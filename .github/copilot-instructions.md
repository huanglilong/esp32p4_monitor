 ## Startup Procedure (CRITICAL)
 ALWAYS at the start of every session:
 1. Read README.md —— Understand hardware information
 2. Read PROJECT.md —— Understand software information
 3. Ensure both files have been read before processing user requests
 4. Schedule a plan before starting any work
 5. when finished all tasks
    - update README.md if necessary
    - update PROJECT.md if necessary
    - update copilot-instructions.md if necessary
    - give a summary of why and how at end, especially for issues and any other important information

# Git
1. with approvement from user, commit and push changes to the repository
2. command: git add . && git commit -m "commit message" && git push
3. commit message should be clear and concise, describing the changes made and useful for future reference, use english language for commit message by default

## ESP-DIF Build, Flash, and Monitor
1. source ~/.espressif/v6.0.1/esp-idf/export.sh
2. idf.py build # Build the project
3. ~~idf.py -p /dev/cu.usbmodem* flash monitor # Flash and monitor~~
4. if changes are made to the code, repeat steps 2-3 to rebuild and flash the updated code
5. don't change the code under `managed_components` folder, only if necessary and allowed by user. It is managed by ESP-DIF and will be overwritten.

## Permission
1. allow read/write access to all files and directories and tools in the workspace
2. allow read only access to files and directories outside the workspace, unless explicitly granted write access
