#!/bin/bash
#
# Script to download Astra SDK image, extract boot subimg, desparsify, and extract kernel
#
set -e

# Configuration
DOWNLOAD_URL="https://github.com/synaptics-astra/sdk/releases/download/scarthgap_6.12_v2.4.0/sl2619_coralboard_scarthgap_6.12_v2.4.0.zip"
OUTPUT_KERNEL="linux"
WORK_DIR="$(pwd)"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

log_info() {
    echo -e "${GREEN}[INFO]${NC} $1" >&2
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1" >&2
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1" >&2
}

# Check dependencies
check_dependencies() {
    local deps=("wget" "unzip" "file" "python3" "lz4")
    for cmd in "${deps[@]}"; do
        if ! command -v "$cmd" &> /dev/null; then
            if [ "$cmd" = "lz4" ]; then
                log_warn "lz4 not found, will attempt to install if needed"
            else
                log_error "Required command '$cmd' not found. Please install it."
                exit 1
            fi
        fi
    done
    
    # Check for simg2img (sparse image tool)
    if ! command -v simg2img &> /dev/null; then
        log_warn "simg2img not found. Trying to use alternative methods."
        # simg2img might not be needed if image is already in raw format
    fi
}

# Download the image
download_image() {
    local zip_file="sl2619_coralboard_scarthgap_6.12_v2.4.0.zip"
    
    if [ -f "$zip_file" ]; then
        log_warn "File $zip_file already exists, skipping download"
        return
    fi
    
    log_info "Downloading image from $DOWNLOAD_URL..."
    wget --progress=bar:force:noscroll -O "$zip_file" "$DOWNLOAD_URL"
    log_info "Download complete: $zip_file"
}

# Find and extract boot image
extract_boot_image() {
    local zip_file="sl2619_coralboard_scarthgap_6.12_v2.4.0.zip"
    
    if [ ! -f "$zip_file" ]; then
        log_error "Zip file not found: $zip_file"
        exit 1
    fi
        
    log_info "Extracting boot image from zip..."
    unzip -o "$zip_file" "eMMCimg/boot.subimg.gz" >/dev/null

    mv eMMCimg/boot.subimg.gz boot.subimg.gz
    gunzip -f boot.subimg.gz
    rmdir eMMCimg
    
    echo "boot.subimg"
}

# Desparsify the image
desparsify_image() {
    local sparse_img="$1"
    local raw_img="boot_raw.img"
    
    if [ ! -f "$sparse_img" ]; then
        log_error "Sparse image not found: $sparse_img"
        exit 1
    fi
    
    # Check if image is actually sparse format
    local file_type=$(file "$sparse_img")
    log_info "Image file type: $file_type"
    
    if echo "$file_type" | grep -q "Android sparse"; then
        log_info "Detected sparse image format, desparsifying..."
        
        if command -v simg2img &> /dev/null; then
            simg2img "$sparse_img" "$raw_img"
            log_info "Desparsified image created: $raw_img"
        else
            log_error "simg2img not found. Install android-tools or python3-android-tools"
            log_info "On Ubuntu/Debian: sudo apt-get install android-tools-fsutils"
            exit 1
        fi
    else
        log_warn "Image doesn't appear to be sparse format, copying as-is..."
        cp "$sparse_img" "$raw_img"
    fi
    
    echo "$raw_img"
}

# Extract kernel from boot image
extract_kernel() {
    local boot_img="$1"
    
    if [ ! -f "$boot_img" ]; then
        log_error "Boot image not found: $boot_img"
        exit 1
    fi
    
    log_info "Extracting kernel..."
    
    # Find LNXB offset using Python
    local lnxb_offset=$(python3 -c "
with open('$boot_img', 'rb') as f:
    data = f.read()
    pos = data.find(b'LNXB')
    if pos >= 0:
        print(pos + 0x40)
    else:
        print(-1)
")
    
    if [ "$lnxb_offset" -lt 0 ]; then
        log_error "Could not find LNXB marker"
        exit 1
    fi
    
    log_info "Kernel starts at offset: $lnxb_offset"
    
    # Extract raw kernel data
    dd if="$boot_img" of="kernel_raw" bs=1 skip="$lnxb_offset" count=50000000 2>/dev/null
    
    # Check file type
    local file_type=$(file kernel_raw)
    log_info "File type: $file_type"
    
    if echo "$file_type" | grep -q "LZ4"; then
        log_info "Decompressing with lz4..."
        lz4 -d kernel_raw "$OUTPUT_KERNEL"
    else
        mv kernel_raw "$OUTPUT_KERNEL"
    fi
    
    # Verify
    local kernel_type=$(file "$OUTPUT_KERNEL")
    log_info "Kernel type: $kernel_type"
    
    if echo "$kernel_type" | grep -q "Linux kernel.*ARM64"; then
        log_info "Successfully extracted kernel"
        rm -f kernel_raw
        return 0
    else
        log_error "Not a valid ARM64 kernel"
        exit 1
    fi
}

# Cleanup intermediate files
cleanup() {
    #local zip_file="sl2619_coralboard_scarthgap_6.12_v2.4.0.zip"
    local boot_files=(*.img)
    
    log_info "Cleaning up intermediate files..."
    
    ## Remove zip file
    #if [ -f "$zip_file" ]; then
    #    rm "$zip_file"
    #    log_info "Removed: $zip_file"
    #fi
    
    # Remove boot images except the final kernel
    for img in "${boot_files[@]}"; do
        if [ -f "$img" ] && [ "$img" != "$OUTPUT_KERNEL" ]; then
            rm "$img"
            log_info "Removed: $img"
        fi
    done
}

# Main execution
main() {
    log_info "Starting kernel extraction process..."
    log_info "Working directory: $WORK_DIR"
    
    check_dependencies
    download_image
    
    local boot_img=$(extract_boot_image)    
    local raw_img=$(desparsify_image "$boot_img")
    extract_kernel "$raw_img"
    
    cleanup
    
    if [ -f "$OUTPUT_KERNEL" ]; then
        log_info "✓ Kernel successfully extracted: $OUTPUT_KERNEL ($(du -h "$OUTPUT_KERNEL" | cut -f1))"
        log_info "Process completed successfully!"
    else
        log_error "Kernel extraction failed!"
        exit 1
    fi
}

# Run main function
main "$@"
