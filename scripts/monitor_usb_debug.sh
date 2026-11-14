#!/usr/bin/env bash

set -u

echo "Waiting for Powerlytics USB CDC debug port..."

while true; do
  port="$(find /dev -maxdepth 1 -name 'cu.usbmodem*' -print 2>/dev/null | head -n 1)"

  if [[ -z "${port}" ]]; then
    sleep 0.2
    continue
  fi

  echo "Connected: ${port}"
  stty -f "${port}" 115200 raw -echo 2>/dev/null || {
    sleep 0.2
    continue
  }

  cat "${port}" 2>/dev/null || true
  echo
  echo "USB reset/disconnected; waiting for re-enumeration..."
  sleep 0.2
done
