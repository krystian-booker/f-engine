"""tools/package_game.py writes its launchers byte for byte on any host
(Windows text files would otherwise turn every newline into CRLF): play.cmd
ends with a single CRLF and the shell script play has no CR at all."""
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import package_game  # noqa: E402

with tempfile.TemporaryDirectory() as folder:
    stage = Path(folder)
    package_game.write_launcher(stage, Path("fengine.exe"), "windows")
    command = (stage / "play.cmd").read_bytes()
    assert command == b'@start "" "%~dp0bin\\fengine.exe" "%~dp0game" %*\r\n', command
    package_game.write_launcher(stage, Path("fengine"), "linux")
    assert b"\r" not in (stage / "play").read_bytes()
