#!/bin/sh
exec "$(dirname "$0")/Satchel.app/Contents/Helpers/satchel" uninstall "$@"
