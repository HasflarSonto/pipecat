#!/bin/bash
# Run Luna on Pi
cd ~/pipecat
source .venv-pi/bin/activate
python pi/luna_pi_debug.py --camera 0
