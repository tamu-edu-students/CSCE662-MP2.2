#!/bin/bash

# # Delay for 10 seconds
# sleep 10

# Kill the specific process
echo "Killing ./tsd -c 2 -s 1..."
pkill -f "./tsd -c 2 -s 1 -h localhost -k 9000 -p 10001"

echo "Process ./tsd -c 2 -s 1 killed!"
