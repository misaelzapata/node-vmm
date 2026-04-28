import { NodeVmmError, parseKeyValueList } from "./utils.js";

export interface ParsedArgs {
  positional: string[];
  values: Map<string, string[]>;
  bools: Set<string>;
}

export function parseOptions(args: string[], booleanFlags: Set<string>, valueFlags?: Set<string>): ParsedArgs {
  const positional: string[] = [];
  const values = new Map<string, string[]>();
  const bools = new Set<string>();

  for (let index = 0; index < args.length; index += 1) {
    const token = args[index];
    if (token === "--") {
      positional.push(...args.slice(index + 1));
      break;
    }
    if (!token.startsWith("--")) {
      positional.push(token);
      continue;
    }

    const withoutPrefix = token.slice(2);
    if (withoutPrefix.startsWith("no-")) {
      const target = withoutPrefix.slice(3);
      if (!booleanFlags.has(target)) {
        throw new NodeVmmError(`unknown flag: --${withoutPrefix}`);
      }
      bools.delete(target);
      continue;
    }

    const equals = withoutPrefix.indexOf("=");
    const name = equals >= 0 ? withoutPrefix.slice(0, equals) : withoutPrefix;
    if (!name) {
      throw new NodeVmmError("empty flag name");
    }
    if (valueFlags && !booleanFlags.has(name) && !valueFlags.has(name)) {
      throw new NodeVmmError(`unknown flag: --${name}`);
    }

    if (booleanFlags.has(name)) {
      bools.add(name);
      if (equals >= 0) {
        const value = withoutPrefix.slice(equals + 1).toLowerCase();
        if (["0", "false", "no", "off"].includes(value)) {
          bools.delete(name);
        }
      }
      continue;
    }

    const value = equals >= 0 ? withoutPrefix.slice(equals + 1) : args[++index];
    if (value === undefined) {
      throw new NodeVmmError(`missing value for --${name}`);
    }
    const existing = values.get(name) ?? [];
    existing.push(value);
    values.set(name, existing);
  }

  return { positional, values, bools };
}

export function stringOption(parsed: ParsedArgs, name: string, fallback = ""): string {
  const values = parsed.values.get(name);
  if (!values || values.length === 0) {
    return fallback;
  }
  return values[values.length - 1];
}

export function stringListOption(parsed: ParsedArgs, name: string): string[] {
  return parsed.values.get(name) ?? [];
}

export function intOption(parsed: ParsedArgs, name: string, fallback: number): number {
  const raw = stringOption(parsed, name, String(fallback));
  const value = Number.parseInt(raw, 10);
  if (!Number.isFinite(value) || value < 0) {
    throw new NodeVmmError(`--${name} must be a non-negative integer`);
  }
  return value;
}

export function boolOption(parsed: ParsedArgs, name: string): boolean {
  return parsed.bools.has(name);
}

export function keyValueOption(parsed: ParsedArgs, name: string): Record<string, string> {
  return parseKeyValueList(stringListOption(parsed, name));
}
