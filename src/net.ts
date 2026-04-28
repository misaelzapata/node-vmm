import net from "node:net";

import { requireCommands, runCommand } from "./process.js";
import type { NetworkConfig, PortForward, PortForwardInput } from "./types.js";
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
  await new Promise<void>((resolve) => {
    server.close(() => resolve());
  });
}

async function setupPortForwards(
  inputs: PortForwardInput[] | undefined,
  guestIp: string,
): Promise<{ ports: PortForward[]; cleanup: () => Promise<void> }> {
  const requested = (inputs ?? []).map(parsePortForward);
  const servers: net.Server[] = [];
  const active: PortForward[] = [];

  try {
    for (const mapping of requested) {
      const server = net.createServer((client) => {
        const upstream = net.connect({ host: guestIp, port: mapping.guestPort });
        const closeBoth = (): void => {
          client.destroy();
          upstream.destroy();
        };
        client.on("error", closeBoth);
        upstream.on("error", closeBoth);
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
      await Promise.all(servers.map((server) => closeServer(server).catch(() => undefined)));
    },
  };
}

export async function setupNetwork(options: SetupNetworkOptions): Promise<NetworkConfig> {
  const mode = options.mode || (options.tapName ? "tap" : "none");
  if (mode === "none" && (options.ports?.length ?? 0) > 0) {
    throw new NodeVmmError("--publish requires --net auto");
  }
  if (mode === "none") {
    return { mode: "none" };
  }

  if (mode === "tap") {
    if ((options.ports?.length ?? 0) > 0) {
      throw new NodeVmmError("--publish requires --net auto");
    }
    if (!options.tapName) {
      throw new NodeVmmError("--net tap requires --tap NAME");
    }
    return {
      mode: "tap",
      ifaceId: "eth0",
      tapName: options.tapName,
      guestMac: deterministicMac(options.id),
    };
  }

  if (mode !== "auto") {
    throw new NodeVmmError(`unsupported network mode: ${mode}`);
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
      kernelIpArg: `ip=${guestIp}::${hostIp}:${netmask}:node-vmm:eth0:off`,
      kernelNetArgs: `node_vmm.iface=eth0 node_vmm.ip=${guestIp}/30 node_vmm.gw=${hostIp} node_vmm.dns=${dns}`,
      ports: portForwards.ports,
      cleanup: cleanupAuto,
    };
  } catch (error) {
    await cleanupAuto();
    throw error;
  }
}
