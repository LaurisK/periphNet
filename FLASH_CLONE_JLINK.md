# J-Link Clone Flashing Solution

## Problem
Clone J-Links show a popup warning that blocks flashing unless manually clicked within ~15 seconds.

## Solution
Use `flash_nokill.sh` which monitors output and kills the process after flash completes, avoiding the popup hang.

## Usage

```bash
# Flash both bootloader and application
./flash_nokill.sh flash_both.jlink

# Flash application only
./flash_nokill.sh flash_application.jlink

# Flash bootloader only
./flash_nokill.sh flash_bootloader.jlink
```

## Performance
- **Before**: 45+ seconds (waiting for manual popup click)
- **After**: ~10 seconds (automated, no intervention needed)

## How It Works
1. Runs JLinkExe with specified script
2. Monitors output for "Verify successful" and "Script processing completed"
3. Kills process immediately after flash completes
4. Popup never gets a chance to block the operation

## Files
- `flash_nokill.sh` - Automated flash script (recommended)
- `flash_both.jlink` - Flash bootloader + application
- `flash_application.jlink` - Flash application only
- `flash_bootloader.jlink` - Flash bootloader only
