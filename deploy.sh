#\!/bin/bash
# Quick deploy script for PocketQuake
cd /home/alberto/Repos/PocketQuake/src/firmware
make && make install && cd ../fpga && make mif && \
cp output_files/ap_core.rbf /run/media/alberto/POCKETDEV/Cores/ThinkElastic.PocketQuake/bitstream.rbf_r && \
cp ../../release/Cores/ThinkElastic.PocketQuake/core.json /run/media/alberto/POCKETDEV/Cores/ThinkElastic.PocketQuake/core.json && \
cp ../firmware/quake.bin /run/media/alberto/POCKETDEV/Assets/pocketquake/common/ && \
sync && echo "Deploy complete"
