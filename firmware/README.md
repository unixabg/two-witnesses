# firmware/ - source of tw_node-s3-ch11.bin, and its desk simulator

tw_node/   ESP-IDF v5.5.6 project (ESP32-S3). Build, from the top of the kit (../Makefile,
           modules: ../docs/MODULES.md):
             make csi_defconfig && make bin       -> out/tw_node-s3-csi-ch11.bin
             make release-bin                     the same, copied to tw_node-s3-ch11.bin (a version bump)
           or by hand, as before:
             . ~/esp-idf/export.sh && cd tw_node && idf.py set-target esp32s3 && idf.py build
             cd build && esptool.py --chip esp32s3 merge_bin -o ../../../tw_node-s3-ch11.bin $(cat flash_args)
           main/tw_proto.c is the protocol (slots, timekeeper, relaying, de-duplication);
           main/main.c is the ESP-IDF glue (Wi-Fi, ESP-NOW, CSI, USB); main/tw_module.h is
           what a module is to it. CSI sensing (records, scores, detections) is a module:
           ../modules/csi/ (docs/MODULES.md). dependencies.lock
           pins esp_csi_gain_ctrl as built.
sim/       the same tw_proto.c in a pretend garage: boards with drifting clocks, a shared
           channel with collisions and loss, boards dying and coming back.
             make sim, make sim-check (from the top of the kit), or here:
             gcc -O2 -I../tw_node/main -o sim sim.c ../tw_node/main/tw_proto.c \
                 ../../modules/csi/tw_mod_csi/csi_sense.c -lm
             ./sim --nodes 3 --seconds 60 > out.txt 2> truth.json && python3 check_sim.py out.txt truth.json
           options: --nodes N  --seconds S  --loss P  --exit 1[,2]  --kill 1@20-40  --walk 30-40
                    --seed N  --realtime (for a pty feeding listen.py)  --no-csi (the core alone)
             --sit A-B[@X-Y][:AMP] (4.7: someone sits and works by link X-Y, 1-2 if not given,
             from A to B s: a steady restlessness, AMP 0.3 unless given, about 4.5x quiet)
             --breathe A-B[@X-Y][:BPM[:AMP]] (someone perfectly still by link X-Y, breathing BPM
             a minute, 15 unless given; AMP 0.02 unless given: the most each subcarrier swings)  --block 2-1@20-100@0.75
                    --setpower T@DBM  --setrelay T@on|off  --rxerr P
                    --set T@second=on|off|force,T@measure=on|off,T@cap=N|off (2.1 settings)
           the second frame: ../docs/SECOND-FRAME.md (design, and the simulator's results)
