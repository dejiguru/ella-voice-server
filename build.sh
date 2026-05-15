#!/bin/bash

# EllaBox AI Robot - Build System with Enhanced UI
# Reconstructed after accidental wipe

FQBN="espressif:esp32:esp32s3:PSRAM=opi,FlashSize=8M,PartitionScheme=default_8MB,UploadSpeed=921600,USBMode=hwcdc,CDCOnBoot=default"
SKETCH="."
BUILD_PATH="./build-ellabox"
CACHE_PATH="./.arduino-cache"
PORT="/dev/ttyACM0"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
MAGENTA='\033[0;35m'
CYAN='\033[0;36m'
WHITE='\033[1;37m'
NC='\033[0m' # No Color
BOLD='\033[1m'

# Spinner function for unknown duration tasks
spinner() {
    local pid=$1
    local message=$2
    local spin='⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏'
    local i=0
    
    echo -ne "${CYAN}${message}${NC} "
    while kill -0 $pid 2>/dev/null; do
        i=$(( (i+1) %10 ))
        printf "\r${CYAN}${message}${NC} ${MAGENTA}${spin:$i:1}${NC}"
        sleep 0.1
    done
    printf "\r${CYAN}${message}${NC} ${GREEN}✓${NC}\n"
}

# Show menu function
show_menu() {
    clear
    echo -e "${BOLD}${MAGENTA}"
    echo "╔════════════════════════════════════════╗"
    echo "║     EllaBox AI Robot - Build Menu      ║"
    echo "╚════════════════════════════════════════╝"
    echo -e "${NC}"
    echo -e "${CYAN}Board:${NC} ESP32-S3 (8MB Flash, OPI PSRAM)"
    echo -e "${CYAN}Port:${NC} $PORT"
    echo ""
    echo -e "${WHITE}1)${NC} ${GREEN}Compile${NC}"
    echo -e "${WHITE}2)${NC} ${YELLOW}Upload${NC}"
    echo -e "${WHITE}3)${NC} ${BLUE}Monitor (Serial)${NC}"
    echo -e "${WHITE}4)${NC} ${MAGENTA}Compile & Upload${NC}"
    echo -e "${WHITE}5)${NC} ${RED}Clean Build${NC}"
    echo -e "${WHITE}q)${NC} Quit"
    echo ""
    echo -ne "${BOLD}Select option: ${NC}"
}

# Main loop
while true; do
    show_menu
    read opt

    case $opt in
        1)
            echo ""
            echo -e "${BOLD}${GREEN}━━━ Compiling EllaBox ━━━${NC}"
            echo ""
            
            # Capture output to temp file
            compile_log=$(mktemp)
            arduino-cli compile --fqbn "$FQBN" --build-path "$BUILD_PATH" --build-cache-path "$CACHE_PATH" --verbose "$SKETCH" > "$compile_log" 2>&1 &
            compile_pid=$!
            spinner $compile_pid "Compiling sketch"
            wait $compile_pid
            compile_status=$?
            
            if [ $compile_status -eq 0 ]; then
                echo ""
                echo -e "${GREEN}✓ Compilation successful!${NC}"
                echo ""
                # Show memory usage
                if [ -f "$BUILD_PATH/ellabox.ino.elf" ]; then
                    echo -e "${CYAN}Memory Usage:${NC}"
                    size "$BUILD_PATH/ellabox.ino.elf" | tail -n 1 | awk '{printf "  RAM:   %s bytes\n  Flash: %s bytes\n", $2+$3, $1}'
                fi
            else
                echo ""
                echo -e "${RED}✗ Compilation failed!${NC}"
                echo ""
                echo -e "${YELLOW}━━━ Searching for errors ━━━${NC}"
                # Show actual error lines
                grep -E "error:|Error|fatal error|undefined reference" "$compile_log" | tail -n 20
                echo ""
                echo -e "${YELLOW}━━━ Last 40 lines of build output ━━━${NC}"
                tail -n 40 "$compile_log"
                echo ""
                echo -e "${YELLOW}━━━ End of error output ━━━${NC}"
                echo ""
                echo -e "${CYAN}Full log saved to: compile_error.log${NC}"
                cp "$compile_log" compile_error.log
            fi
            
            rm -f "$compile_log"
            echo ""
            echo -ne "${CYAN}Press Enter to continue...${NC}"
            read
            ;;
        2)
            echo ""
            if [ ! -f "$BUILD_PATH/ellabox.ino.bin" ]; then
                echo -e "${RED}✗ No compiled binary found. Compile first!${NC}"
                echo ""
                echo -ne "${CYAN}Press Enter to continue...${NC}"
                read
                continue
            fi
            
            echo -e "${BOLD}${YELLOW}━━━ Uploading to ESP32 ━━━${NC}"
            echo ""
            
            # Check if device is connected
            if [ ! -e "$PORT" ]; then
                echo -e "${RED}✗ Device not found at $PORT${NC}"
                echo -e "${YELLOW}  Please connect your ESP32 and try again${NC}"
                echo ""
                echo -ne "${CYAN}Press Enter to continue...${NC}"
                read
                continue
            fi
            
            arduino-cli upload -p "$PORT" --fqbn "$FQBN" --input-dir "$BUILD_PATH" --verbose &
            upload_pid=$!
            spinner $upload_pid "Uploading firmware"
            wait $upload_pid
            upload_status=$?
            
            if [ $upload_status -eq 0 ]; then
                echo ""
                echo -e "${GREEN}✓ Upload successful!${NC}"
                echo -e "${CYAN}  Device is ready at $PORT${NC}"
            else
                echo ""
                echo -e "${RED}✗ Upload failed!${NC}"
                echo -e "${YELLOW}  Check USB connection and try again${NC}"
            fi
            
            echo ""
            echo -ne "${CYAN}Press Enter to continue...${NC}"
            read
            ;;
        3)
            echo ""
            echo -e "${BOLD}${BLUE}━━━ Serial Monitor ━━━${NC}"
            echo -e "${CYAN}Press Ctrl+C to exit${NC}"
            echo ""
            sleep 1
            arduino-cli monitor -p "$PORT" -c baudrate=115200 --timestamp
            ;;
        4)
            echo ""
            echo -e "${BOLD}${MAGENTA}━━━ Compile & Upload ━━━${NC}"
            echo ""
            
            # Compile
            echo -e "${GREEN}Step 1/2: Compiling...${NC}"
            
            # Capture output to temp file
            compile_log=$(mktemp)
            arduino-cli compile --fqbn "$FQBN" --build-path "$BUILD_PATH" --build-cache-path "$CACHE_PATH" --verbose "$SKETCH" > "$compile_log" 2>&1 &
            compile_pid=$!
            spinner $compile_pid "Compiling sketch"
            wait $compile_pid
            compile_status=$?
            
            if [ $compile_status -ne 0 ]; then
                echo ""
                echo -e "${RED}✗ Compilation failed!${NC}"
                echo ""
                echo -e "${YELLOW}━━━ Searching for errors ━━━${NC}"
                # Show actual error lines
                grep -E "error:|Error|fatal error|undefined reference" "$compile_log" | tail -n 20
                echo ""
                echo -e "${YELLOW}━━━ Last 40 lines of build output ━━━${NC}"
                tail -n 40 "$compile_log"
                echo ""
                echo -e "${YELLOW}━━━ End of error output ━━━${NC}"
                echo ""
                echo -e "${CYAN}Full log saved to: compile_error.log${NC}"
                cp "$compile_log" compile_error.log
                rm -f "$compile_log"
                echo ""
                echo -ne "${CYAN}Press Enter to continue...${NC}"
                read
                continue
            fi
            
            rm -f "$compile_log"
            echo -e "${GREEN}✓ Compilation successful${NC}"
            echo ""
            
            # Ask for confirmation before upload
            echo -ne "${YELLOW}Upload to device? (y/n): ${NC}"
            read -n 1 confirm
            echo ""
            
            if [ "$confirm" != "y" ] && [ "$confirm" != "Y" ]; then
                echo -e "${CYAN}Upload cancelled${NC}"
                echo ""
                echo -ne "${CYAN}Press Enter to continue...${NC}"
                read
                continue
            fi
            
            echo ""
            # Upload
            echo -e "${YELLOW}Step 2/2: Uploading...${NC}"
            
            # Check if device is connected
            if [ ! -e "$PORT" ]; then
                echo -e "${RED}✗ Device not found at $PORT${NC}"
                echo -e "${YELLOW}  Please connect your ESP32 and try again${NC}"
                echo ""
                echo -ne "${CYAN}Press Enter to continue...${NC}"
                read
                continue
            fi
            
            arduino-cli upload -p "$PORT" --fqbn "$FQBN" --input-dir "$BUILD_PATH" --verbose &
            upload_pid=$!
            spinner $upload_pid "Uploading firmware"
            wait $upload_pid
            upload_status=$?
            
            if [ $upload_status -eq 0 ]; then
                echo ""
                echo -e "${GREEN}✓ Upload successful!${NC}"
                echo -e "${CYAN}  Device is ready at $PORT${NC}"
                echo ""
                echo -ne "${BLUE}Start serial monitor? (y/n): ${NC}"
                read -n 1 monitor_confirm
                echo ""
                
                if [ "$monitor_confirm" = "y" ] || [ "$monitor_confirm" = "Y" ]; then
                    echo ""
                    echo -e "${BOLD}${BLUE}━━━ Serial Monitor ━━━${NC}"
                    echo -e "${CYAN}Press Ctrl+C to exit${NC}"
                    echo ""
                    sleep 1
                    arduino-cli monitor -p "$PORT" -c baudrate=115200 --timestamp
                fi
            else
                echo ""
                echo -e "${RED}✗ Upload failed!${NC}"
                echo -e "${YELLOW}  Check USB connection and try again${NC}"
            fi
            
            echo ""
            echo -ne "${CYAN}Press Enter to continue...${NC}"
            read
            ;;
        5)
            echo ""
            echo -e "${BOLD}${RED}━━━ Cleaning Build ━━━${NC}"
            echo ""
            if [ -d "$BUILD_PATH" ] || [ -d "$CACHE_PATH" ]; then
                rm -rf "$BUILD_PATH" "$CACHE_PATH"
                echo -e "${GREEN}✓ Build artifacts cleaned${NC}"
                echo -e "${CYAN}  Removed: $BUILD_PATH${NC}"
                echo -e "${CYAN}  Removed: $CACHE_PATH${NC}"
            else
                echo -e "${YELLOW}  No build artifacts to clean${NC}"
            fi
            
            echo ""
            echo -ne "${CYAN}Press Enter to continue...${NC}"
            read
            ;;
        q|Q)
            echo ""
            echo -e "${CYAN}Goodbye!${NC}"
            exit 0
            ;;
        *)
            echo ""
            echo -e "${RED}✗ Invalid option${NC}"
            sleep 1
            ;;
    esac
done
