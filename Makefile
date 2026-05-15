BUILD_DIR := budz/build/esp32.esp32.esp32s3
FIRMWARE_DIR := web/firmware

.PHONY: firmware

# In Arduino IDE: Sketch > Export Compiled Binary (Alt+Ctrl+S), then run this.
firmware:
	cp $(BUILD_DIR)/budz.ino.bootloader.bin $(FIRMWARE_DIR)/bootloader.bin
	cp $(BUILD_DIR)/budz.ino.partitions.bin $(FIRMWARE_DIR)/partitions.bin
	cp $(BUILD_DIR)/budz.ino.bin            $(FIRMWARE_DIR)/firmware.bin
	@echo "Firmware copied to $(FIRMWARE_DIR)/"
