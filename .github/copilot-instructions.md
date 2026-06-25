 ## Startup Procedure (CRITICAL)
 ALWAYS at the start of every session:
 1. Read README.md —— Understand hardware information
 2. Read project_setup.md —— Understand software information
 3. Ensure both files have been read before processing user requests
 4. Schedule a plan before starting any work
 5. Update project_setup.md if necessary

## ESP-DIF Build, Flash, and Monitor
1. source ~/.espressif/v6.0.1/esp-idf/export.sh
2. idf.py build # Build the project
3. idf.py -p /dev/cu.usbmodem* flash monitor # Flash and monitor
4. if changes are made to the code, repeat steps 2-3 to rebuild and flash the updated code
