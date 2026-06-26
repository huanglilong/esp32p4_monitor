 ## Startup Procedure (CRITICAL)
 ALWAYS at the start of every session:
 1. Read README.md —— Understand hardware information
 2. Read project_setup.md —— Understand software information
 3. Ensure both files have been read before processing user requests
 4. Schedule a plan before starting any work
 5. when finished all tasks
    - update README.md if necessary
    - update project_setup.md if necessary
    - update copilot-instructions.md if necessary
    - give a summary of why and how at end, especially for issues and any other important information

## ESP-DIF Build, Flash, and Monitor
1. source ~/.espressif/v6.0.1/esp-idf/export.sh
2. idf.py build # Build the project
3. ~~idf.py -p /dev/cu.usbmodem* flash monitor # Flash and monitor~~
4. if changes are made to the code, repeat steps 2-3 to rebuild and flash the updated code

## Permission
1. allow read/write access to all files and directories and tools in the workspace
2. allow read only access to files and directories outside the workspace, unless explicitly granted write access
