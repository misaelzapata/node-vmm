import { existsSync } from "node:fs";
import net from "node:net";

import { requireCommands, runCommand } from "./process.js";
import type { NetworkConfig, PortForward, PortForwardInput, SlirpHostFwd } from "./types.js";
import { NodeVmmError, compactIdForTap, deterministicMac, requireRoot } from "./utils.js";

export interface SetupNetworkOptions {
  id: string;
  mode: string;
  tapName?: string;
  ports?: PortForwardInput[];
}

function randomSubnetOctet(id: string): number {
  let sum = 0;
  for (const char of id) {
    sum = (sum + char.charCodeAt(0)) % 140;
  }
  return 80 + sum;
}

function socketVmnetPath(): string | undefined {
  const configured = process.env.NODE_VMM_SOCKET_VMNET;
  if (configured) {
    return configured;
  }
  const candidates = [
    "/opt/socket_vmnet/socket_vmnet",
    "/var/run/socket_vmnet",
    "/opt/homebrew/var/run/socket_vmnet",
    "/usr/local/var/run/socket_vmnet",
  ];
  return candidates.find((candidate) => existsSync(candidate));
}

function macNetworkBackend(): "slirp" | "socket_vmnet" | "vmnet" {
  const backend = (process.env.NODE_VMM_HVF_NET_BACKEND || process.env.NODE_VMM_MAC_NET || "slirp").trim();
  if (backend === "socket_vmnet" || backend === "vmnet" || backend === "slirp") {
    return backend;
  }
  throw new NodeVmmError(`unsupported macOS HVF network backend: ${backend}`);
}

function validatePort(port: number, label: string, options: { allowZero?: boolean } = {}): number {
  const min = options.allowZero ? 0 : 1;
  if (!Number.isInteger(port) || port < min || port > 65535) {
    throw new NodeVmmError(`${label} must be a TCP port from ${min} to 65535`);
  }
  return port;
}

function parsePortNumber(input: string, label: string, options: { allowZero?: boolean } = {}): number {
  if (!/^\d+$/.test(input)) {
    throw new NodeVmmError(`${label} must be a TCP port number`);
  }
  return validatePort(Number.parseInt(input, 10), label, options);
}

function stripTcpProtocol(input: string): string {
  const slash = input.lastIndexOf("/");
  if (slash < 0) {
    return input;
  }
  const protocol = input.slice(slash + 1).toLowerCase();
  if (protocol !== "tcp") {
    throw new NodeVmmError(`node-vmm only supports TCP port publishing, got ${protocol}`);
  }
  return input.slice(0, slash);
}

export function parsePortForward(input: PortForwardInput): PortForward {
  if (typeof input === "number") {
    const port = validatePort(input, "port");
    return { host: "127.0.0.1", hostPort: 0, guestPort: port };
  }
  if (typeof input !== "string") {
    return {
      host: input.host || "127.0.0.1",
      hostPort: validatePort(input.hostPort, "hostPort", { allowZero: true }),
      guestPort: validatePort(input.guestPort, "guestPort"),
    };
  }
  const spec = stripTcpProtocol(input.trim());
  if (!spec) {
    throw new NodeVmmError("invalid --publish mapping: empty value");
  }
  const parts = spec.split(":");
  if (parts.length === 1) {
    const port = parsePortNumber(parts[0], "port");
    return { host: "127.0.0.1", hostPort: 0, guestPort: port };
  }
  if (parts.length === 2) {
    return {
      host: "127.0.0.1",
      hostPort: parsePortNumber(parts[0], "hostPort", { allowZero: true }),
      guestPort: parsePortNumber(parts[1], "guestPort"),
    };
  }
  if (parts.length === 3) {
    return {
      host: parts[0] || "127.0.0.1",
      hostPort: parsePortNumber(parts[1], "hostPort", { allowZero: true }),
      guestPort: parsePortNumber(parts[2], "guestPort"),
    };
  }
  throw new NodeVmmError(`invalid --publish mapping: ${input}`);
}

async function listen(server: net.Server, host: string, port: number): Promise<number> {
  await new Promise<void>((resolve, reject) => {
    const onError = (error: Error): void => {
      server.off("listening", onListening);
      reject(error);
    };
    const onListening = (): void => {
      server.off("error", onError);
      resolve();
    };
    server.once("error", onError);
    server.once("listening", onListening);
    server.listen(port, host);
  });
  const address = server.address();
  if (!address || typeof address === "string") {
    return port;
  }
  return address.port;
}

async function closeServer(server: net.Server): Promise<void> {
  if (!server.listening) {
    return;
  }
  await new Promise<void>((resolve) => {
    server.close(() => resolve());
  });
}

async function resolveHostPort(host: string, port: number): Promise<number> {
  if (port !== 0) {
    return port;
  }
  const server = net.createServer();
  try {
    return await listen(server, host, 0);
  } finally {
    await closeServer(server);
  }
}

async function setupPortForwards(
  inputs: PortForwardInput[] | undefined,
  guestIp: string,
): Promise<{ ports: PortForward[]; cleanup: () => Promise<void> }> {
  const requested = (inputs ?? []).map(parsePortForward);
  const servers: net.Server[] = [];
  const sockets = new Set<net.Socket>();
  const active: PortForward[] = [];

  try {
    for (const mapping of requested) {
      const server = net.createServer((client) => {
        const upstream = net.connect({ host: guestIp, port: mapping.guestPort });
        sockets.add(client);
        sockets.add(upstream);
        client.once("close", () => sockets.delete(client));
        upstream.once("close", () => sockets.delete(upstream));
        let closing = false;
        const closeBoth = (): void => {
          if (closing) {
            return;
          }
          closing = true;
          client.destroy();
          upstream.destroy();
        };
        client.on("error", closeBoth);
        upstream.on("error", closeBoth);
        client.on("close", closeBoth);
        upstream.on("close", closeBoth);
        client.pipe(upstream);
        upstream.pipe(client);
      });
      const host = mapping.host || "127.0.0.1";
      const hostPort = await listen(server, host, mapping.hostPort);
      servers.push(server);
      active.push({ host, hostPort, guestPort: mapping.guestPort });
    }
  } catch (error) {
    await Promise.all(servers.map((server) => closeServer(server).catch(() => undefined)));
    throw error;
  }

  return {
    ports: active,
    cleanup: async () => {
      const closed = Promise.all(servers.map((server) => closeServer(server).catch(() => undefined)));
      for (const socket of sockets) {
        socket.destroy();
      }
      await closed;
    },
  };
}

async function allocatePortForwards(inputs: PortForwardInput[] | undefined): Promise<PortForward[]> {
  const requested = (inputs ?? []).map(parsePortForward);
  const active: PortForward[] = [];
  for (const mapping of requested) {
    const host = mapping.host || "127.0.0.1";
    if (mapping.hostPort !== 0) {
      active.push({ host, hostPort: mapping.hostPort, guestPort: mapping.guestPort });
      continue;
    }
    const server = net.createServer();
    try {
      const hostPort = await listen(server, host, 0);
      active.push({ host, hostPort, guestPort: mapping.guestPort });
    } finally {
      await closeServer(server);
    }
  }
  return active;
}

export async function setupNetwork(options: SetupNetworkOptions): Promise<NetworkConfig> {
  let mode = options.mode || (options.tapName ? "tap" : "none");
  // On Windows/WHP and macOS/HVF, "auto" resolves to libslirp user-mode
  // networking. On Linux/KVM, "auto" stays as the TAP+NAT path further below.
  if (mode === "auto" && process.platform === "win32") {
    mode = "slirp";
  }
  if (mode === "auto" && process.platform === "darwin" && macNetworkBackend() === "slirp") {
    mode = "slirp";
  }
  if (mode === "none" && (options.ports?.length ?? 0) > 0) {
    throw new NodeVmmError("--publish requires --net auto or --net slirp");
  }
  if (mode === "none") {
    return { mode: "none" };
  }

  if (mode === "tap") {
    if ((options.ports?.length ?? 0) > 0) {
      throw new NodeVmmError("--publish requires --net auto or --net slirp");
    }
    if (!options.tapName) {
      throw new NodeVmmError("--net tap requires --tap NAME");
    }
    if (process.platform === "darwin") {
      throw new NodeVmmError("macOS/HVF does not support arbitrary --net tap devices; use --net auto for slirp networking");
    }
    return {
      mode: "tap",
      ifaceId: "eth0",
      tapName: options.tapName,
      guestMac: deterministicMac(options.id),
    };
  }

  if (mode === "slirp") {
    // libslirp uses a fixed 10.0.2.0/24 layout; the guest gets 10.0.2.15,
    // host is reachable at 10.0.2.2, DNS at 10.0.2.3. Port forwarding goes
    // through libslirp's hostfwd, so the host-side TCP proxy used for `auto`
    // mode is not required.
    const hostFwds: SlirpHostFwd[] = [];
    for (const p of (options.ports ?? []).map(parsePortForward)) {
      const hostAddr = p.host || "127.0.0.1";
      hostFwds.push({
        udp: false,
        hostAddr,
        hostPort: await resolveHostPort(hostAddr, p.hostPort),
        guestPort: p.guestPort,
      });
    }
    return {
      mode: "slirp",
      ifaceId: "eth0",
      tapName: process.platform === "darwin" ? "slirp" : undefined,
      guestMac: deterministicMac(options.id),
      guestIp: "10.0.2.15",
      hostIp: "10.0.2.2",
      netmask: "255.255.255.0",
      cidrPrefix: 24,
      dns: "10.0.2.3",
      cidr: "10.0.2.15/24",
      // Skip the kernel-side ip= autoconf because it forces the kernel to
      // wait for the eth0 carrier; the userspace init script in src/rootfs.ts
      // brings up the interface itself once it sees node_vmm.iface=...
      kernelNetArgs:
        "node_vmm.iface=eth0 node_vmm.ip=10.0.2.15/24 node_vmm.gw=10.0.2.2 node_vmm.dns=10.0.2.3",
      ports: hostFwds.map((h) => ({
        host: h.hostAddr,
        hostPort: h.hostPort,
        guestPort: h.guestPort,
      })),
      hostFwds,
      cleanup: async () => {},
    };
  }

  if (mode !== "auto") {
    throw new NodeVmmError(`unsupported network mode: ${mode}`);
  }

  // macOS: prefer QEMU-style user networking through libslirp. vmnet/socket_vmnet
  // remain available through NODE_VMM_HVF_NET_BACKEND for privileged setups.
  if (process.platform === "darwin") {
    const backend = macNetworkBackend();
    const vmnetSocket = backend === "socket_vmnet" ? socketVmnetPath() : undefined;
    if (backend === "socket_vmnet" && !vmnetSocket) {
      throw new NodeVmmError("NODE_VMM_HVF_NET_BACKEND=socket_vmnet requires NODE_VMM_SOCKET_VMNET or a socket_vmnet socket");
    }
    const slirp = backend === "slirp";
    const octet = randomSubnetOctet(options.id);
    const hostIp = slirp ? "10.0.2.2" : `172.31.${octet}.1`;
    const guestIp = slirp ? "10.0.2.15" : `172.31.${octet}.2`;
    const netmask = slirp ? "255.255.255.0" : "255.255.255.252";
    const cidrPrefix = slirp ? 24 : 30;
    const dns = process.env.NODE_VMM_GUEST_DNS || (slirp ? "10.0.2.3" : "1.1.1.1");
    const portForwards = slirp ? undefined : await setupPortForwards(options.ports, guestIp);
    const ports = slirp ? await allocatePortForwards(options.ports) : portForwards?.ports;
    return {
      mode: slirp ? "slirp" : "tap",
      ifaceId: "eth0",
      tapName: slirp ? "slirp" : (vmnetSocket ? `socket_vmnet:${vmnetSocket}` : "vmnet:shared"),
      guestMac: deterministicMac(options.id),
      hostIp,
      guestIp,
      netmask,
      cidrPrefix,
      dns,
      kernelIpArg: `ip=${guestIp}::${hostIp}:${netmask}:node-vmm:eth0:off`,
      kernelNetArgs: `node_vmm.iface=eth0 node_vmm.ip=${guestIp}/${cidrPrefix} node_vmm.gw=${hostIp} node_vmm.dns=${dns}`,
      ports,
      cleanup: portForwards?.cleanup ?? (async () => undefined),
    };
  }

  requireRoot("--net auto");
  await requireCommands(["ip", "sysctl", "iptables"]);

  const tapName = options.tapName || compactIdForTap(options.id);
  const octet = randomSubnetOctet(options.id);
  const hostIp = `172.31.${octet}.1`;
  const guestIp = `172.31.${octet}.2`;
  const netmask = "255.255.255.252";
  const cidr = `${hostIp}/30`;
  const dns = process.env.NODE_VMM_GUEST_DNS || "1.1.1.1";

  const createdRules: string[][] = [];
  let portForwards: { ports: PortForward[]; cleanup: () => Promise<void> } | undefined;
  const cleanupAuto = async (): Promise<void> => {
    await portForwards?.cleanup();
    for (const rule of createdRules.reverse()) {
      const deleteRule = rule.map((part, index) => (index === 2 && part === "-A" ? "-D" : part));
      if (rule[0] === "-A") {
        deleteRule[0] = "-D";
      }
      await runCommand("iptables", deleteRule, { allowFailure: true, capture: true });
    }
    await runCommand("ip", ["link", "delete", tapName], { allowFailure: true, capture: true });
  };

  try {
    await runCommand("ip", ["tuntap", "add", "dev", tapName, "mode", "tap"]);
    await runCommand("ip", ["addr", "add", cidr, "dev", tapName]);
    await runCommand("ip", ["link", "set", "dev", tapName, "up"]);
    await runCommand("sysctl", ["-w", "net.ipv4.ip_forward=1"], { allowFailure: true });

    const natRule = ["-t", "nat", "-A", "POSTROUTING", "-s", `${guestIp}/32`, "-j", "MASQUERADE"];
    const fwdIn = ["-A", "FORWARD", "-i", tapName, "-j", "ACCEPT"];
    const fwdOut = [
      "-A",
      "FORWARD",
      "-o",
      tapName,
      "-m",
      "state",
      "--state",
      "RELATED,ESTABLISHED",
      "-j",
      "ACCEPT",
    ];
    for (const rule of [natRule, fwdIn, fwdOut]) {
      await runCommand("iptables", rule);
      createdRules.push(rule);
    }
    portForwards = await setupPortForwards(options.ports, guestIp);

    return {
      mode: "tap",
      ifaceId: "eth0",
      tapName,
      guestMac: deterministicMac(options.id),
      hostIp,
      guestIp,
      netmask,
      cidrPrefix: 30,
      dns,
      kernelNetArgs: `node_vmm.iface=eth0 node_vmm.ip=${guestIp}/30 node_vmm.gw=${hostIp} node_vmm.dns=${dns}`,
      ports: portForwards.ports,
      cleanup: cleanupAuto,
    };
  } catch (error) {
    await cleanupAuto();
    throw error;
  }
}
