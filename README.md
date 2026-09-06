# loadavg-watch

Monitor de carga del sistema en tiempo real desde `/proc/loadavg`.

## Features

- **Load averages** 1m/5m/15m con ratio por CPU
- **Procesos**: running, sleeping, stopped, zombie
- **Memoria**: usage, available, buffers, cached, dirty pages
- **Watch mode**: refresco periódico con delta tracking
- **Snapshot JSON**: salida estructurada para scripting
- **Sin dependencias**: solo C++23 stdlib + `/proc`

## Build

```bash
g++ -std=c++23 -O2 -Wall -Wextra src/main.cpp -o loadavg-watch
```

## Usage

```bash
./loadavg-watch              # Watch mode (3s refresh)
./loadavg-watch -w 5         # Custom interval
./loadavg-watch --snapshot   # JSON output
./loadavg-watch --selftest   # Self-tests
./loadavg-watch -c           # No color
```

## Output

Watch mode shows:
- Load averages with per-core ratio and color coding
- Process state breakdown
- Memory usage with dirty pages
- Visual load bars with delta tracking

Colors: green (<0.7x) cyan (<1x) yellow (<2x) red (>=2x per core)

## License

MIT
