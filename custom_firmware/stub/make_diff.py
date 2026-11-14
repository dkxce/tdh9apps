file_path = "stub.patch"
with open(file_path, "r", encoding="utf-8") as f:
    lines = f.readlines()
lines[2] = "--- a/src/stub.s\n"
lines[3] = "--- b/src/stub.s\n"
with open(file_path, "w", encoding="utf-8") as f:
    f.writelines(lines)
