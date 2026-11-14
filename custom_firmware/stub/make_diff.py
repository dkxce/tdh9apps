import subprocess
import sys
from pathlib import Path

here = Path(__file__).resolve().parent
r = subprocess.run(
    ["git", "diff", "--no-index", "--", "stub_base.s", "stub.s"],
    cwd=here,
    capture_output=True,
)
raw = r.stdout.decode("utf-8", errors="replace")
if r.returncode not in (0, 1):
    sys.stderr.write(r.stderr.decode("utf-8", errors="replace"))
    raise SystemExit("git diff failed: %d" % r.returncode)
raw = raw.replace("a/stub_base.s", "a/src/stub.s").replace("b/stub.s", "b/src/stub.s")
raw = raw.replace("--- a/stub_base.s", "--- a/src/stub.s").replace("+++ b/stub.s", "+++ b/src/stub.s")
(here / "stub.patch").write_text(raw, encoding="utf-8", newline="\n")
print("wrote stub.patch %d bytes" % (here / "stub.patch").stat().st_size)
