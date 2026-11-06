# firmware/ - source of tw_node-s3-ch11.bin, and its desk simulator

tw_node/   ESP-IDF v5.5.6 project (ESP32-S3). Build:
             . ~/esp-idf/export.sh && cd tw_node && idf.py set-target esp32s3 && idf.py build
             cd build && esptool.py --chip esp32s3 merge_bin -o ../../../tw_node-s3-ch11.bin $(cat flash_args)
           main/tw_proto.c is the protocol (slots, timekeeper, relaying, de-duplication);
           main/main.c is the ESP-IDF glue (Wi-Fi, ESP-NOW, CSI, USB). dependencies.lock
           pins esp_csi_gain_ctrl as built.
sim/       the same tw_proto.c in a pretend garage: boards with drifting clocks, a shared
           channel with collisions and loss, boards dying and coming back.
             gcc -O2 -o sim sim.c ../tw_node/main/tw_proto.c -lm
             ./sim --nodes 3 --seconds 60 > out.txt 2> truth.json && python3 check_sim.py out.txt truth.json
           options: --nodes N  --seconds S  --loss P  --exit 1[,2]  --kill 1@20-40  --walk 30-40
                    --seed N  --realtime (for a pty feeding listen.py)  --block 2-1@20-100@0.75
                    --setpower T@DBM  --setrelay T@on|off  --rxerr P
                    --set T@second=on|off|force,T@measure=on|off,T@cap=N|off (2.1 settings)
           the second frame: ../docs/SECOND-FRAME.md (design, and the simulator's results)
