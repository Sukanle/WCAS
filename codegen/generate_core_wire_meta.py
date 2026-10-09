#!/usr/bin/env python3
"""Generate Wayland core wire metadata for the proxy.

`wayland.xml` is not a complete wire description: `wayland-scanner` fills some
arguments in from context. `wl_registry.bind` is declared there with only
`name` and `id`, while the wire message is
`bind(name: uint, interface: string, version: uint, id: new_id) = "usun"`.
Both `struct wl_registry_interface` (server header) and the client-side
`wl_registry_bind` marshaller in `wayland-client-protocol.h` show all four.

So the wire signature comes from the scanner-generated **server** header, the
message name and version come from `wayland.xml`, and the `new_id` interface
comes from a small table matching what the scanner knows but the server header
does not spell out.

Usage:
    generate_core_wire_meta.py wayland.xml wayland-server-protocol.h OUTPUT_CPP
"""

import re
import sys
import xml.etree.ElementTree as ET

# `new_id` arguments that create a fixed interface. This mirrors
# wayland-server-core.h's dispatch table. `wl_registry.bind` is the one the
# server header cannot express because the interface is a runtime string.
NEW_ID_INTERFACES = {
    ("wl_display", "sync"): "wl_callback",
    ("wl_display", "get_registry"): "wl_registry",
    ("wl_registry", "bind"): "",  # interface chosen by the client at runtime
    ("wl_compositor", "create_surface"): "wl_surface",
    ("wl_compositor", "create_region"): "wl_region",
    ("wl_subcompositor", "get_subsurface"): "wl_subsurface",
    ("wl_shm", "create_pool"): "wl_shm_pool",
    ("wl_shm_pool", "create_buffer"): "wl_buffer",
    ("wl_seat", "get_pointer"): "wl_pointer",
    ("wl_seat", "get_keyboard"): "wl_keyboard",
    ("wl_seat", "get_touch"): "wl_touch",
    ("wl_data_device_manager", "get_data_device"): "wl_data_device",
    ("wl_data_device_manager", "create_data_source"): "wl_data_source",
    ("wl_data_device", "get_data_offer"): "wl_data_offer",
    ("wl_data_source", "offer"): "wl_data_offer",
    ("wl_surface", "frame"): "wl_callback",
    ("wl_pointer", "get_cursor"): "wl_surface",
}

SCALAR_TYPES = {
    "int": "i",
    "int32_t": "i",
    "uint": "u",
    "uint32_t": "u",
    "fixed": "f",
    "wl_fixed_t": "f",
    "string": "s",
    "array": "a",
    "fd": "h",
}


def strip_comments(text):
    return re.sub(r"/\*.*?\*/", "", text, flags=re.S)


def request_handlers(header, interface):
    """`{request name: wire parameter list}` for an interface's server struct.

    A server handler is declared as
    `void (*<name>)(struct wl_client *, struct wl_resource *, <wire args>)`, so
    the wire arguments are everything after the first two parameters.
    """
    block = interface_block(header, r"struct %s_interface\s*\{(.*?)\n\};" % re.escape(interface))
    handlers = {}
    for match in re.finditer(r"\(\*(\w+)\)\(([^;]*?)\)\s*;", block, re.S):
        parameters = split_parameters(match.group(2))
        handlers[match.group(1)] = [wire_code(parameter) for parameter in parameters[2:]]
    return handlers


def split_parameters(block):
    parameters = []
    depth = 0
    current = ""
    for character in block:
        if character == "(":
            depth += 1
        elif character == ")":
            depth -= 1
        if character == "," and depth == 0:
            parameters.append(current)
            current = ""
        else:
            current += character
    if current.strip():
        parameters.append(current)
    return parameters


def wire_code(parameter):
    parameter = parameter.strip()
    if not parameter or parameter == "void":
        return None
    if "*" in parameter:
        if parameter.startswith("const char") or parameter.startswith("char"):
            return "s"
        if "array" in parameter:
            return "a"
        return "o"
    tokens = parameter.split()
    if len(tokens) < 2:
        return None
    type_text = " ".join(tokens[:-1])
    code = SCALAR_TYPES.get(type_text)
    if code is None:
        raise SystemExit(f"unknown parameter type {type_text!r} in {parameter!r}")
    return code


def interface_block(text, struct_pattern):
    match = re.search(struct_pattern, text, re.S)
    return strip_comments(match.group(1)) if match else ""


def resolve_new_interface(name, message_name, arguments):
    """Interface created by a `new_id` argument.

    The authoritative source is the `interface` attribute on the `new_id` arg
    in `wayland.xml`. Events such as `wl_data_device.data_offer` and
    `wl_surface.get_release` name it directly and are absent from
    `NEW_ID_INTERFACES`, so relying on the table alone left their created
    objects without a role and tore the connection down. `wl_registry.bind`
    deliberately leaves it to a runtime string (mapped to "" here).
    """
    for argument in arguments:
        if argument.get("type") == "new_id":
            interface = argument.get("interface")
            if interface:
                return interface
    return NEW_ID_INTERFACES.get((name, message_name))


def main():
    if len(sys.argv) != 4:
        print(__doc__, file=sys.stderr)
        return 2
    xml_path, header_path, output_path = sys.argv[1], sys.argv[2], sys.argv[3]

    root = ET.parse(xml_path).getroot()
    with open(header_path) as handle:
        header = handle.read()

    interfaces = []
    for element in root.findall("interface"):
        name = element.get("name")
        interface = {
            "name": name,
            "version": int(element.get("version", "1")),
            "requests": [],
            "events": [],
        }
        # The server header spells out the real wire parameters per request.
        handlers = request_handlers(header, name)
        for opcode, request in enumerate(element.findall("request")):
            declared = request.findall("arg")
            request_name = request.get("name")
            if (name, request_name) == ("wl_registry", "bind"):
                # wayland.xml intentionally omits the runtime interface and
                # version arguments. They are part of the actual wire
                # protocol and are present in the generated server handler.
                signature = "usun"
            elif request_name in handlers:
                # The server handler loses wire-level distinctions: `new_id`
                # is exposed as uint32_t and `fd` as int32_t. Restore those
                # types from XML while retaining the server header for the
                # parameters that XML/scanner context does not fully spell
                # out.
                codes = handlers[request_name]
                for index, argument in enumerate(declared):
                    if index >= len(codes):
                        break
                    if argument.get("type") == "new_id":
                        codes[index] = "n"
                    elif argument.get("type") == "fd":
                        codes[index] = "h"
                signature = "".join(code for code in codes if code)
            else:
                # No server handler (for example wl_registry.bind is present,
                # but a client-only request may not be): fall back to the XML.
                signature = ""
                for argument in declared:
                    code = {
                        "int": "i", "uint": "u", "fixed": "f", "string": "s",
                        "object": "o", "new_id": "n", "array": "a", "fd": "h",
                    }.get(argument.get("type"))
                    if code is None:
                        raise SystemExit(f"unknown arg type in {name}.{request.get('name')}")
                    if argument.get("allow-null", "false") == "true" and code in ("o", "s"):
                        signature += "?"
                    signature += code
            interface["requests"].append(
                {
                    "name": request.get("name"),
                    "signature": signature,
                    "new_interface": resolve_new_interface(name, request.get("name"), declared),
                    "since": int(request.get("since", "1")),
                    "opcode": opcode,
                }
            )
        for opcode, event in enumerate(element.findall("event")):
            signature = ""
            event_arguments = event.findall("arg")
            for argument in event_arguments:
                code = {
                    "int": "i", "uint": "u", "fixed": "f", "string": "s",
                    "object": "o", "new_id": "n", "array": "a", "fd": "h",
                }.get(argument.get("type"))
                if code is None:
                    raise SystemExit(f"unknown arg type in {name}.{event.get('name')}")
                if argument.get("allow-null", "false") == "true" and code in ("o", "s"):
                    signature += "?"
                signature += code
            interface["events"].append(
                {
                    "name": event.get("name"),
                    "signature": signature,
                    "new_interface": resolve_new_interface(name, event.get("name"), event_arguments),
                    "since": int(event.get("since", "1")),
                    "opcode": opcode,
                }
            )
        interfaces.append(interface)

    with open(output_path, "w") as output:
        output.write("// Generated by codegen/generate_core_wire_meta.py -- DO NOT EDIT.\n")
        output.write('#include "wire_meta.h"\n\n')
        output.write("namespace skl::wcas::proxy::wire {\n\n")
        for interface in interfaces:
            symbol = interface["name"]
            if interface["requests"]:
                output.write(f"const MessageMeta {symbol}_requests[] = {{\n")
                for message in interface["requests"]:
                    output.write(
                        "    {%s, %s, %s, %u, %u},\n"
                        % (
                            _string(message["name"]),
                            _string(message["signature"]),
                            _string(message["new_interface"]),
                            message["since"],
                            message["opcode"],
                        )
                    )
                output.write("};\n\n")
            if interface["events"]:
                output.write(f"const MessageMeta {symbol}_events[] = {{\n")
                for message in interface["events"]:
                    output.write(
                        "    {%s, %s, %s, %u, %u},\n"
                        % (
                            _string(message["name"]),
                            _string(message["signature"]),
                            _string(message["new_interface"]),
                            message["since"],
                            message["opcode"],
                        )
                    )
                output.write("};\n\n")
        output.write("static const InterfaceMeta core_table[] = {\n")
        for interface in interfaces:
            symbol = interface["name"]
            output.write(
                "    {%s, %u, %s, %u, %s, %u},\n"
                % (
                    _string(interface["name"]),
                    interface["version"],
                    f"{symbol}_requests" if interface["requests"] else "nullptr",
                    len(interface["requests"]),
                    f"{symbol}_events" if interface["events"] else "nullptr",
                    len(interface["events"]),
                )
            )
        output.write("};\n\n")
        output.write(f"const std::size_t interface_count = {len(interfaces)};\n\n")
        output.write("""const InterfaceMeta* core_interfaces() {
    return core_table;
}

std::size_t core_interface_count() {
    return interface_count;
}

const InterfaceMeta* find(const char* name) {
    if (!name) return nullptr;
    for (std::size_t i = 0; i < interface_count; ++i) {
        const InterfaceMeta& candidate = core_table[i];
        std::size_t index = 0;
        while (candidate.name[index] != '\\0' && candidate.name[index] == name[index]) ++index;
        if (candidate.name[index] == '\\0' && name[index] == '\\0') return &candidate;
    }
    const InterfaceMeta* extension = extension_interfaces();
    const std::size_t extension_count = extension_interface_count();
    for (std::size_t i = 0; i < extension_count; ++i) {
        const InterfaceMeta& candidate = extension[i];
        std::size_t index = 0;
        while (candidate.name[index] != '\\0' && candidate.name[index] == name[index]) ++index;
        if (candidate.name[index] == '\\0' && name[index] == '\\0') return &candidate;
    }
    return nullptr;
}

} // namespace skl::wcas::proxy::wire
""")
    return 0


def _string(value):
    if value is None:
        return "nullptr"
    return '"%s"' % value.replace("\\", "\\\\").replace('"', '\\"')


if __name__ == "__main__":
    sys.exit(main())
