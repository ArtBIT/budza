#!/usr/bin/env bash
cd "$(dirname "$0")"
echo "Open http://localhost:8080 in Chrome or Edge (WebSerial required for flashing)"
python3 -m http.server 8080
