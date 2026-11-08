# EMBER: early fire and gas-leak detection (IoT Spark 4.0, team Cappucin)

| Folder / file | What it is |
|---|---|
| `server.py`, `config.py`, `templates/` | server, dashboard and security |
| `ember_esp32/`, `link_test/` | ESP32 firmware and link test |
| `fake_esp32.py`, `attack_test.py` | simulator and attack tests |
| `camera/` | computer vision: flame score from the webcam, YOLO model, camera metrics (see `camera/README.md`) |
| `docs/` | spec book, team brief, planning sheet and context |

Secrets (`ember_secrets.py`, `*/secrets.h`) are not in git: copy the `*.example` files and fill them in from the team chat.
