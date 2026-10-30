# CSI board-to-board firmware for the Hosyond ESP32-S3 boards

> **Corrected 2026-10-26 (kit v4.5).** The jobs and settings below are the
> ones in use: one sender, one receiver with **v2** firmware, about
> **91 packets/s**. **The receiver is read on the board's
> native USB port** (lsusb `303a:1001 Espressif USB JTAG/serial`,
> `/dev/ttyACM0`), not the COM/UART port: the firmware copies its output
> there, and the baud setting is ignored on that port. It gives 87/s with
> 4% loss (A/B of 2026-10-25); keep it there so runs stay comparable. The
> first receiver image (921600 baud) is kept only for reference.

Built 2026-10-22 from Espressif's own examples (the sender and the first
receiver unchanged; v2 receiver: UART 2,000,000 baud and log level WARN):

- esp-csi commit `8633d67152db2808f141cc1595970aa9cf406045` (2026-04-22),
  `examples/get-started/csi_send` and `csi_recv`
- ESP-IDF v5.5.6, target esp32s3
- Each file is one merged image: flash it at address 0x0.

```
febf998d0bddbb79c1df1131e35294662d33cb4c83ea160d696ef055458bbb3b  csi_send-s3-ch11.bin
f97b93aac6377334b16192473d6c02a0435ad9d2540784756653e2090f710e1a  csi_recv-s3-ch11-v2.bin   (in use)
d52018928909ce5451cb27a30392c4240cffe5b5b6537fcf857c3fb28c0732e2  csi_recv-s3-ch11.bin      (first, 921600)
```

## What it does

- **Sender** (one board): broadcasts one small ESP-NOW packet about 91
  times a second (the example asks for 100) on **channel 11**, 40 MHz wide (channels 7-11; an access point on channel 1
  is clear of it). Each packet carries a counting number. It uses a made-up
  address, 1a:00:00:00:00:00.
- **Receiver** (another board, v2): listens on channel 11, keeps only packets
  from that address, and prints one line per packet (on the COM/UART port
  at 2,000,000 baud, and the same on the native USB port, which is the one
  in use):
  `CSI_DATA,id,mac,rssi,rate,...,local_timestamp,...,len,first_word,"[raw CSI]"`
  - `id` = the sender's counting number (gaps = lost packets)
  - `local_timestamp` = receiver's clock in microseconds
  - `rssi` = signal in dBm; the bracket = raw CSI bytes (pairs of numbers
    per subcarrier)
- Habits to expect: it sets its gain baseline from the first 100 packets,
  so skip the first ~10 s of every run (analyze_csi.py does). The first
  receiver also printed `compensate_gain ...` lines; v2 does not.

## Flash (each board, once)

```
cd ~/Downloads
sha256sum csi_send-s3-ch11.bin csi_recv-s3-ch11-v2.bin     # compare with above
```
Board in download mode (hold BOOT, tap RST, release BOOT), then:
```
sudo ~/scratch-esp/bin/esptool --chip esp32s3 --port /dev/ttyACM0 erase-flash
sudo ~/scratch-esp/bin/esptool --chip esp32s3 --port /dev/ttyACM0 write-flash 0x0 csi_send-s3-ch11.bin
```
(Repeat erase then write for the receiver with `csi_recv-s3-ch11-v2.bin`.)
Tap RST afterwards to start the new program.

## First look

1. Sender: plug into any USB power (either port).
2. Receiver: plug its **native USB** port into the Chromebook, directly
   (a 10 ft passive extension lost 63% of the packets).
3. Watch it live (Ctrl-] to quit):
   ```
   sudo ~/scratch-esp/bin/python -m serial.tools.miniterm /dev/ttyACM0 2000000
   ```
   You should see `CSI_DATA,...` lines scrolling about 90 per second.

## Record

Set the clock first (chrony), then:
```
sudo ~/scratch-esp/bin/python record_csi.py /dev/ttyACM0 shakedown-01 60
```
It writes `shakedown-01.csv` (each line starts with the Linux time in
nanoseconds) and ends with a count of lost packets. For real runs use
`short_run.sh` and `run.sh` (see README.md).
