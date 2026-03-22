import re
import sys

def process(file_path):
    with open(file_path, 'r', encoding='utf-8') as f:
        content = f.read()

    # Find the M0X0 switch block
    m0x0_match = re.search(r'switch\(Op\) {\s+(case 0x00:.*?)^\s*\}\s*\}[\r\n]+^\s*else if \(Opcodes == S9xOpcodesM1X0\)', content, re.MULTILINE | re.DOTALL)
    if not m0x0_match:
        print("Could not find M0X0 block!")
        return

    m0x0_cases = m0x0_match.group(1).strip().split('\n')
    m0x0_table = []
    m0x0_labels = []
    for line in m0x0_cases:
        line = line.strip()
        if not line: continue
        match = re.match(r'case (0x[0-9A-Fa-f]{2}): (Op[a-zA-Z0-9_]+)\(\); break;', line)
        if match:
            hex_val = match.group(1)
            func_name = match.group(2)
            m0x0_table.append(f"&&L_m0_{hex_val}")
            m0x0_labels.append(f"L_m0_{hex_val}: {func_name}(); goto M0X0_END;")

    m0x0_replacement = (
        "#if defined(__GNUC__) || defined(__clang__)\n"
        "\t\t\tstatic const void *dispatch_table_m0[256] = {\n\t\t\t\t" +
        ", ".join(m0x0_table) + "\n\t\t\t};\n"
        "\t\t\tgoto *dispatch_table_m0[Op];\n\t\t\t" +
        "\n\t\t\t".join(m0x0_labels) + "\n"
        "\t\t\tM0X0_END: ;\n"
        "#else\n"
        "\t\t\tswitch(Op) {\n\t\t\t" + "\n\t\t\t".join(m0x0_cases) + "\n\t\t\t}\n"
        "#endif"
    )

    content = content[:m0x0_match.start()] + m0x0_replacement + "\n\t\t}\n\t\telse if (Opcodes == S9xOpcodesM1X0)" + content[m0x0_match.end():]

    # Find the M1X0 block
    m1x0_match = re.search(r'else if \(Opcodes == S9xOpcodesM1X0\)[\r\n\s]+\{[\r\n\s]+switch\(Op\) {\s+(case 0x00:.*?)^\s*\}\s*\}', content, re.MULTILINE | re.DOTALL)
    if not m1x0_match:
        print("Could not find M1X0 block!")
        return

    m1x0_cases = m1x0_match.group(1).strip().split('\n')
    m1x0_table = []
    m1x0_labels = []
    for line in m1x0_cases:
        line = line.strip()
        if not line: continue
        match = re.match(r'case (0x[0-9A-Fa-f]{2}): (Op[a-zA-Z0-9_]+)\(\); break;', line)
        if match:
            hex_val = match.group(1)
            func_name = match.group(2)
            m1x0_table.append(f"&&L_m1_{hex_val}")
            m1x0_labels.append(f"L_m1_{hex_val}: {func_name}(); goto M1X0_END;")

    m1x0_replacement = (
        "else if (Opcodes == S9xOpcodesM1X0)\n\t\t{\n"
        "#if defined(__GNUC__) || defined(__clang__)\n"
        "\t\t\tstatic const void *dispatch_table_m1[256] = {\n\t\t\t\t" +
        ", ".join(m1x0_table) + "\n\t\t\t};\n"
        "\t\t\tgoto *dispatch_table_m1[Op];\n\t\t\t" +
        "\n\t\t\t".join(m1x0_labels) + "\n"
        "\t\t\tM1X0_END: ;\n"
        "#else\n"
        "\t\t\tswitch(Op) {\n\t\t\t" + "\n\t\t\t".join(m1x0_cases) + "\n\t\t\t}\n"
        "#endif\n\t\t}"
    )

    content = content[:m1x0_match.start()] + m1x0_replacement + content[m1x0_match.end():]

    with open(file_path, 'w', encoding='utf-8') as f:
        f.write(content)

    print("Successfully replaced switch statements with computed gotos in cpuexec.cpp")

if __name__ == "__main__":
    process(r"c:\snes\snes9x-v0.5\cpuexec.cpp")

