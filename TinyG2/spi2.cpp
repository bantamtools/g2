#include "tinyg2.h"					// #1 There are some dependencies
#include "config.h"					// #2
#include "canonical_machine.h"
#include "hardware.h"
#include "spi2.h"
#include "util.h"
#include "motate/utility/SamSPI.h"
#include "motate/MotateTimers.h"
#include "text_parser.h"

#include <stdio.h>
#include <memory>

// Buffers for passing data to SPI2 command processor
uint8_t wbuf[SPI2_BUF_SIZE] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

uint8_t rbuf[SPI2_BUF_SIZE] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// SPI2 Slave Interrupt Flag
static volatile bool spi2_slave_int = false;

// Create a SPI instance using CS1 pin for select
std::shared_ptr<Motate::SPI<kSocket2_SPISlaveSelectPinNumber>> spi2;

// Encoder data global variable (for JSON commands)
float spi2_encoder_pos[SPI2_NUM_AXES] = {0.0,0.0,0.0,0.0};

// Encoder, User IO and Interlock value global variables (for JSON commands)
uint8_t spi2_enc_idx = 0;
uint8_t spi2_io_idx = 0;
uint8_t spi2_io_val = 0;
uint8_t spi2_itr_idx = 0;
uint8_t spi2_itr_val = 0;

// Spindle LED global variable (for JSON commands)
uint8_t spi2_spd_led[5] = {0,0,0,0,0};

// Epsilon global variables (for JSON commands)
uint8_t spi2_eps_axis = 0;
float spi2_eps_val = 0.0;

// Firmware version global variable (for JSON commands)
struct spi2_fw_type spi2_fw_ver = {0, 0, 0};

//TEMP: Calibrated delay function
void delay_us(uint32_t us) {
  for(uint32_t i = 0; i < us * 5; i++); // Calibrated empirically
}
// spi2_init: Initializes the SPI2 subsystem
void spi2_init() {

  // Pin direction, type, debounce set in hardware.h

  // Initialize the SPI peripheral and set to this channel
  spi2.reset(new Motate::SPI<kSocket2_SPISlaveSelectPinNumber>(SPI2_MCK_DIV));

  // Add delays for 1) SS low to SCLK and 2) between transfers for SPI slave
  spi2->setBSDelay(SPI2_DLYBS_US);
  spi2->setBCTDelay(SPI2_DLYBCT_US);

  // Set up highest priority interrupt on falling edge of SPI_CS2 pin
  spi2_int_pin.setInterrupts(kPinInterruptOnFallingEdge|kPinInterruptPriorityHighest);

  // Clear Slave Interrupt Flag
  spi2_slave_int = false;

}

// spi2_cmd: Start a SPI master transfer (read/write) using the provided buffers
uint8_t spi2_cmd(bool slave_req, uint8_t cmd_byte, uint8_t *wr_buf, uint16_t wr_cnt, uint8_t *rd_buf, uint16_t rd_cnt) {

  int i = 0;
  int16_t ret;
  uint8_t sts_byte = SPI2_STS_ERR;
  uint32_t start_time;

  // Check that incoming buffers properly initialized if needed
  if ((wr_cnt > 0) && (!wr_buf)) {
    fprintf_P(stderr, PSTR("\nERROR: Write data buffer not initialized properly\n"));
    return SPI2_STS_ERR;
  } else if ((rd_cnt > 0) && (!rd_buf)) {
    fprintf_P(stderr, PSTR("\nERROR: Read data buffer not initialized properly\n"));
    return SPI2_STS_ERR;
  }

  // Command error checking, immediately return error on malformed command
  switch (cmd_byte) {

    // Single byte master write commands (0x01, 0x02)
    case SPI2_CMD_RST_ENC_POS:
    case SPI2_CMD_START_TOOL_TIP:

      if ((slave_req) || (wr_cnt > 0) || (rd_cnt > 0)) {

        fprintf_P(stderr, PSTR("\nERROR: Malformed single byte write (Command 0x%02X)\n"),cmd_byte);
        return SPI2_STS_ERR;
      }
      break;

    // Multi byte master read commands (0x04)
    case SPI2_CMD_REQ_ENC_POS:

      if ((slave_req) || (wr_cnt > 0) || (rd_cnt != (SPI2_NUM_AXES * 4))) {
        fprintf_P(stderr, PSTR("\nERROR: Malformed multi byte read (Command 0x%02X)\n"),cmd_byte);
        return SPI2_STS_ERR;
      }
      break;

    // Slave-requested write commands (0x03)
    case SPI2_CMD_SND_MTR_POS:

      if ((!slave_req) || (wr_cnt != (SPI2_NUM_AXES * 4)) || (rd_cnt > 0)) {
        fprintf_P(stderr, PSTR("\nERROR: Malformed slave-requested write (Command 0x%02X)\n"),cmd_byte);
        return SPI2_STS_ERR;
      }
      break;

    // Read Encoder Position command (0x40)
    case SPI2_CMD_RD_ENC_POS:

      if ((slave_req) || (wr_buf[0] > AXIS_A) || (wr_cnt != 1) || (rd_cnt != 4)) {
        fprintf_P(stderr, PSTR("\nERROR: Malformed Read Encoder Position command (Command 0x%02X)\n"),cmd_byte);
        return SPI2_STS_ERR;
      }
      break;

    // User IO commands (0x41, 0x42)
    case SPI2_CMD_SET_USER_IO:
    case SPI2_CMD_CLR_USER_IO:

      if ((slave_req) || (wr_buf[0] > 6) || (wr_cnt != 1) || (rd_cnt > 0)) {

        fprintf_P(stderr, PSTR("\nERROR: Malformed User IO command (Command 0x%02X)\n"),cmd_byte);
        return SPI2_STS_ERR;
      }
      break;

    // Read User IO command (0x43)
    case SPI2_CMD_RD_USER_IO:

      if ((slave_req) || (wr_buf[0] > 6) || (wr_cnt != 1) || (rd_cnt != 1)) {

        fprintf_P(stderr, PSTR("\nERROR: Malformed Read User IO command (Command 0x%02X)\n"),cmd_byte);
        return SPI2_STS_ERR;
      }
      break;

    // User LED commands (0x44, 0x45)
    case SPI2_CMD_SET_USER_LED:
    case SPI2_CMD_CLR_USER_LED:

      if ((slave_req) || (wr_buf[0] > 2) || (wr_cnt != 1) || (rd_cnt > 0)) {

        fprintf_P(stderr, PSTR("\nERROR: Malformed User LED command (Command 0x%02X)\n"),cmd_byte);
        return SPI2_STS_ERR;
      }
      break;

    // Read Interlock Loop command (0x46)
    case SPI2_CMD_RD_ITR_LOOP:

      if ((slave_req) || (wr_buf[0] > 1) || (wr_cnt != 1) || (rd_cnt != 1)) {

        fprintf_P(stderr, PSTR("\nERROR: Malformed Read Interlock Loop command (Command 0x%02X)\n"),cmd_byte);
        return SPI2_STS_ERR;
      }
      break;

    // Set Spindle LED command (0x47)
    case SPI2_CMD_SET_SPIN_LED:

      if ((slave_req) || (wr_buf[0] > 2) || (wr_cnt != 5) || (rd_cnt > 0)) {

        fprintf_P(stderr, PSTR("\nERROR: Malformed Set Spindle LED command (Command 0x%02X)\n"),cmd_byte);
        return SPI2_STS_ERR;
      }
      break;

    // Set Epsilon Command (0x48)
    case SPI2_CMD_SET_EPS:

      if ((slave_req) || (wr_buf[0] > AXIS_A) || (wr_cnt != 5) || (rd_cnt > 0)) {

        fprintf_P(stderr, PSTR("\nERROR: Malformed Set Epsilon command (Command 0x%02X)\n"),cmd_byte);
        return SPI2_STS_ERR;
      }
      break;

    // Read ESC Current command (0x49) skipped (TODO - implement)

    // Firmware Version command (0x4A)
    case SPI2_CMD_FW_VER:

      if ((slave_req) || (wr_cnt > 0) || (rd_cnt != 3)) {

        fprintf_P(stderr, PSTR("\nERROR: Malformed Firmware Version command (Command 0x%02X)\n"),cmd_byte);
        return SPI2_STS_ERR;
      }
      break;

    // Not a command, default to error
    default:

      fprintf_P(stderr, PSTR("\nERROR: Invalid command 0x%02X\n"),cmd_byte);
      return SPI2_STS_ERR;
  }

  // Attempt command up to the specified number of retries or when OK status
  for (i = 0; (i < SPI2_NUM_RETRIES && sts_byte != SPI2_STS_OK); i++) {

    // Select the SS for the SPI2 slave
    spi2->setChannel();

    // Write out command byte (slave request, skip this)
    if (!slave_req) {
      spi2->write(cmd_byte, true);
      // Wait for TXEMPTY to flush, then clear unused RX data without invoking read
      start_time = SysTickTimer_getValue();
      while(!spi2->is_tx_empty() && ((SysTickTimer_getValue() - start_time) < SPI2_TIMEOUT));
      while(spi2->is_rx_ready() && ((SysTickTimer_getValue() - start_time) < SPI2_TIMEOUT)) {
        spi2->read();
      }
      // Timed out, report and exit with timeout status
      if ((SysTickTimer_getValue() - start_time) >= SPI2_TIMEOUT) {
        fprintf_P(stderr, PSTR("\nERROR: Timed out waiting on command byte\n"));
        return SPI2_STS_TIMEOUT;
      }
      // Request Encoder Positions command requires time for SPI2 to prep data - TODO fix performance
      if (cmd_byte == SPI2_CMD_REQ_ENC_POS) {
        delay(4);
      } else {
        delay_us(25);
      }
    }

    // Process data bytes if available; process write data, then read data
    if (wr_cnt > 0) {
      spi2->write(wr_buf, wr_cnt, true);
      // Wait for TXEMPTY to flush, then clear unused RX data without invoking read
      start_time = SysTickTimer_getValue();
      while(!spi2->is_tx_empty() && ((SysTickTimer_getValue() - start_time) < SPI2_TIMEOUT));
      while(spi2->is_rx_ready() && ((SysTickTimer_getValue() - start_time) < SPI2_TIMEOUT)) {
        // Commands with multiple writes and reads need time to prep data - TODO fix performance
        if (rd_cnt > 0) {
          delay(1);
        }
        spi2->read();
      }
      // Timed out, report and exit with timeout status
      if ((SysTickTimer_getValue() - start_time) >= SPI2_TIMEOUT) {
        fprintf_P(stderr, PSTR("\nERROR: Timed out waiting on data bytes\n"));
        return SPI2_STS_TIMEOUT;
      }
      // Commands with multiple writes and reads need time to prep data - TODO fix performance
      if (rd_cnt > 0) {
        delay(1);
      } else {
        delay_us(25);
      }
    }
    if (rd_cnt > 0) {
      spi2->read(rd_buf, rd_cnt, true);   // Read RX data (performs dummy writes, doesn't count if RDRF = 0)
      delay_us(25);
    }
    delay_us(25);

    // Read the status
    start_time = SysTickTimer_getValue();
    while (((ret = spi2->read(true)) < 0) && ((SysTickTimer_getValue() - start_time) < SPI2_TIMEOUT)) {  // Waits until RX ready to read (performs dummy writes)
      delay_us(25);
    }
    // Timed out, report and exit with timeout status
    if ((SysTickTimer_getValue() - start_time) >= SPI2_TIMEOUT) {
      fprintf_P(stderr, PSTR("\nERROR: Timed out waiting on status byte\n"));
      return SPI2_STS_TIMEOUT;
    }

    // Convert return to status byte
    sts_byte = (uint8_t)(ret & 0x00FF);

    // Flush the system in case leftovers in buffers
    spi2->flush();
  }

  // Exceeded number of retries, report and exit with retries status
  if (i >= SPI2_NUM_RETRIES) {
    fprintf_P(stderr, PSTR("\nERROR: Maximum number of retries exceeded\n"));
    return SPI2_STS_RETRIES;
  }

  // Return the status code
  return sts_byte;
}

// spi2_slave_handler: Processes SPI2 slave interrupts and executes commands
uint8_t spi2_slave_handler() {

  uint8_t cmd, status;
  uint32_t start_time;
  int16_t ret;

  // SPI2 Slave Interrupt received
  if (spi2_slave_int) {

    delay_us(25);

    // Clear flag
    spi2_slave_int = false;

    // Disable Interrupts
	  //__disable_irq();

    // Enable SPI2 slave select
    spi2->setChannel();

    // Read command
    start_time = SysTickTimer_getValue();
    while (((ret = spi2->read(true)) < 0) && ((SysTickTimer_getValue() - start_time) < SPI2_TIMEOUT)) {  // Waits until RX ready to read (performs dummy writes)
      delay_us(25);
    }
    // Timed out, report and exit with timeout status
    if ((SysTickTimer_getValue() - start_time) >= SPI2_TIMEOUT) {
      fprintf_P(stderr, PSTR("\nERROR: Timed out waiting on slave command byte\n"));
      return SPI2_STS_TIMEOUT;
    }

    // Convert return to command
    cmd = (uint8_t)(ret & 0x00FF);

    switch (cmd) {

      // Send motor positions (slave requested write)
      case SPI2_CMD_SND_MTR_POS:

        status = spi2_send_motor_positions();
        break;

      // Else, error (no other valid slave commands)
      default:

        fprintf_P(stderr, PSTR("\nERROR: Invalid command from SPI2 slave\n"));
        status = SPI2_STS_ERR;
        break;
    }

    // Enable Interrupts
    //__enable_irq();

  } else {
    status = SPI2_STS_OK;
  }

  return status;
}

// spi2_reset_encoder_positions: reset encoder positions (command 0x01)
uint8_t spi2_reset_encoder_positions() {
  return (spi2_cmd(false, SPI2_CMD_RST_ENC_POS, NULL, 0, NULL, 0));
}

// spi2_start_tool_tip: start tool tip command (command 0x02)
uint8_t spi2_start_tool_tip() {
  return (spi2_cmd(false, SPI2_CMD_START_TOOL_TIP, NULL, 0, NULL, 0));
}

// spi2_send_motor_positions: send motor positions (command 0x03)
uint8_t spi2_send_motor_positions() {

  float f;
  uint32_t u;

  // Get each motor position, convert to uint32_t and store bytes into buffer
  for (uint8_t axis = AXIS_X; axis < SPI2_NUM_AXES; axis++) {

    // Convert from float to unsigned 32-bit integer
    f = cm_get_g28_position(axis);
    u = FLOAT_TO_U32(f);

    // Break 32-bits into separate bytes
    wbuf[axis*4] = (uint8_t)((u >> 24) & 0xFF);
    wbuf[axis*4+1] = (uint8_t)((u >> 16) & 0xFF);
    wbuf[axis*4+2] = (uint8_t)((u >> 8) & 0xFF);
    wbuf[axis*4+3] = (uint8_t)(u & 0xFF);

  }

  // Write out buffer with motor position data to SPI slave and return status
  return(spi2_cmd(true, SPI2_CMD_SND_MTR_POS, wbuf, (SPI2_NUM_AXES*4), NULL, 0));
}

// spi2_request_encoder_positions: request encoder positions (command 0x04)
uint8_t spi2_request_encoder_positions() {

  uint8_t st;
  uint32_t u;

  // Get the encoder position data as one 16-byte transfer
  st = spi2_cmd(false, SPI2_CMD_REQ_ENC_POS, NULL, 0, rbuf, (SPI2_NUM_AXES*4));

  // Convert the data in the buffer to their approriate array values
  for (uint8_t axis = AXIS_X; axis < SPI2_NUM_AXES; axis++) {
    u = ((rbuf[axis*4] << 24) + (rbuf[axis*4+1] << 16) + (rbuf[axis*4+2] << 8) + (rbuf[axis*4+3]));
    spi2_encoder_pos[axis] = U32_TO_FLOAT(u);
  }

  return st;
}

// spi2_read_encoder_position: read single encoder position (command 0x40 / 64)
uint8_t spi2_read_encoder_position(uint8_t axis) {

  uint8_t st;
  uint32_t u;

  // Save off index for text mode print
  spi2_enc_idx = axis;

  // Get the encoder position data as one 4-byte transfer
  wbuf[0] = axis;
  st = spi2_cmd(false, SPI2_CMD_RD_ENC_POS, wbuf, 1, rbuf, 4);

  // Convert the data in the buffer to their approriate array value
  u = ((rbuf[axis*4] << 24) + (rbuf[axis*4+1] << 16) + (rbuf[axis*4+2] << 8) + (rbuf[axis*4+3]));
  spi2_encoder_pos[axis] = U32_TO_FLOAT(u);

  return st;
}

// spi2_set_user_io: set user io (command 0x41 / 65)
uint8_t spi2_set_user_io(uint8_t idx) {
  wbuf[0] = idx;
  return(spi2_cmd(false, SPI2_CMD_SET_USER_IO, wbuf, 1, rbuf, 0));
}

// spi2_clear_user_io: clear user io (command 0x42 / 66)
uint8_t spi2_clear_user_io(uint8_t idx) {
  wbuf[0] = idx;
  return(spi2_cmd(false, SPI2_CMD_CLR_USER_IO, wbuf, 1, rbuf, 0));
}

// spi2_read_user_io: read user io (command 0x43 / 67)
uint8_t spi2_read_user_io(uint8_t idx) {

  uint8_t st;

  // Save off index for text mode print
  spi2_io_idx = idx;

  // Read IO value and store in global variable
  wbuf[0] = idx;
  st = spi2_cmd(false, SPI2_CMD_RD_USER_IO, wbuf, 1, rbuf, 1);

  spi2_io_val = rbuf[0];

  return st;
}

// spi2_set_user_led: set user led (command 0x44 / 68)
uint8_t spi2_set_user_led(uint8_t idx) {
  wbuf[0] = idx;
  return(spi2_cmd(false, SPI2_CMD_SET_USER_LED, wbuf, 1, rbuf, 0));
}

// spi2_clear_user_led: clear user led (command 0x45 / 69)
uint8_t spi2_clear_user_led(uint8_t idx) {
  wbuf[0] = idx;
  return(spi2_cmd(false, SPI2_CMD_CLR_USER_LED, wbuf, 1, rbuf, 0));
}

// spi2_read_itr_loop: read interlock loop (command 0x46 / 70)
uint8_t spi2_read_itr_loop(uint8_t idx) {

  uint8_t st;

  // Save off index for text mode print
  spi2_itr_idx = idx;

  // Read interlock loop value and store in global variable
  wbuf[0] = idx;
  st = spi2_cmd(false, SPI2_CMD_RD_ITR_LOOP,  wbuf, 1, rbuf, 1);

  spi2_itr_val = rbuf[0];

  return st;
}

// spi2_set_spindle_led: set spindle led strip (command 0x47 / 71)
uint8_t spi2_set_spindle_led() {

  wbuf[0] = spi2_spd_led[SPI2_SPD_IDX];
  wbuf[1] = spi2_spd_led[SPI2_SPD_R];
  wbuf[2] = spi2_spd_led[SPI2_SPD_G];
  wbuf[3] = spi2_spd_led[SPI2_SPD_B];
  wbuf[4] = spi2_spd_led[SPI2_SPD_W];
  return(spi2_cmd(false, SPI2_CMD_SET_SPIN_LED,  wbuf, 5, rbuf, 0));
}

// spi2_set_epsilon: set epsilon (command 0x48 / 72)
uint8_t spi2_set_epsilon() {

  uint32_t u;

  // Set index and value for global variables
  wbuf[0] = spi2_eps_axis;

  u = FLOAT_TO_U32(spi2_eps_val);

  // Break 32-bits into separate bytes
  wbuf[1] = (uint8_t)((u >> 24) & 0xFF);
  wbuf[2] = (uint8_t)((u >> 16) & 0xFF);
  wbuf[3] = (uint8_t)((u >> 8) & 0xFF);
  wbuf[4] = (uint8_t)(u & 0xFF);

  return(spi2_cmd(false, SPI2_CMD_SET_EPS, wbuf, 5, rbuf, 0));
}

// spi2_read_esc_current: read esc current (command 0x49 / 73) - TODO implement

// spi2_get_fw_version: firmware version (command 0x4a / 74)
uint8_t spi2_get_fw_version() {

  uint8_t st;

  // Read the firmware version and store in global variables
  st = spi2_cmd(false, SPI2_CMD_FW_VER,  wbuf, 0, rbuf, 3);

  spi2_fw_ver.major = rbuf[0];
  spi2_fw_ver.minor = rbuf[1];
  spi2_fw_ver.rev   = rbuf[2];

  return st;
}

// spi2_test: unit testing for SPI2 interface
void spi2_test() {

  spi2->setChannel();

  uint8_t *null_buf = 0;

  fprintf_P(stderr, PSTR("\n*** SPI2 Test BEGIN ***\n"));

  // NULL Buffers
  fprintf_P(stderr, PSTR("\nSending commands w/ NULL buffers...\n"));
  spi2_cmd(false, SPI2_CMD_SND_MTR_POS, null_buf, (SPI2_NUM_AXES*4), rbuf, 0);
  spi2_cmd(false, SPI2_CMD_REQ_ENC_POS, wbuf, 0, null_buf, (SPI2_NUM_AXES*4));

  // Malformed commands (shouldn't appear on bus)
  fprintf_P(stderr, PSTR("\nSending malformed commands...\n"));
  spi2_cmd(true,  SPI2_CMD_RST_ENC_POS, wbuf, 0, rbuf, 0);
  spi2_cmd(false, SPI2_CMD_RST_ENC_POS, wbuf, 10, rbuf, 0);
  spi2_cmd(false, SPI2_CMD_RST_ENC_POS, wbuf, 0, rbuf, 10);

  spi2_cmd(true,  SPI2_CMD_START_TOOL_TIP, wbuf, 0, rbuf, 0);
  spi2_cmd(false, SPI2_CMD_START_TOOL_TIP, wbuf, 10, rbuf, 0);
  spi2_cmd(false, SPI2_CMD_START_TOOL_TIP, wbuf, 0, rbuf, 10);

  spi2_cmd(false, SPI2_CMD_SND_MTR_POS, wbuf, (SPI2_NUM_AXES*4), rbuf, 0);
  spi2_cmd(true,  SPI2_CMD_SND_MTR_POS, wbuf, 0, rbuf, (SPI2_NUM_AXES*4));
  spi2_cmd(true,  SPI2_CMD_SND_MTR_POS, wbuf, (SPI2_NUM_AXES*4), rbuf, (SPI2_NUM_AXES*4));

  spi2_cmd(true,  SPI2_CMD_REQ_ENC_POS, wbuf, 0, rbuf, (SPI2_NUM_AXES*4));
  spi2_cmd(false, SPI2_CMD_REQ_ENC_POS, wbuf, (SPI2_NUM_AXES*4), rbuf, 0);
  spi2_cmd(false, SPI2_CMD_REQ_ENC_POS, wbuf, (SPI2_NUM_AXES*4), rbuf, (SPI2_NUM_AXES*4));

  wbuf[0] = 0;
  spi2_cmd(true,  SPI2_CMD_RD_ENC_POS, wbuf, 1, rbuf, 4);
  spi2_cmd(false, SPI2_CMD_RD_ENC_POS, wbuf, 10, rbuf, 4);
  spi2_cmd(false, SPI2_CMD_RD_ENC_POS, wbuf, 1, rbuf, 10);
  wbuf[0] = 0x80;
  spi2_cmd(false, SPI2_CMD_RD_ENC_POS, wbuf, 1, rbuf, 4);

  wbuf[0] = 0;
  spi2_cmd(true,  SPI2_CMD_SET_USER_IO, wbuf, 1, rbuf, 0);
  spi2_cmd(false, SPI2_CMD_SET_USER_IO, wbuf, 10, rbuf, 0);
  spi2_cmd(false, SPI2_CMD_SET_USER_IO, wbuf, 1, rbuf, 10);
  wbuf[0] = 0x80;
  spi2_cmd(false, SPI2_CMD_SET_USER_IO, wbuf, 1, rbuf, 0);

  wbuf[0] = 0;
  spi2_cmd(true,  SPI2_CMD_CLR_USER_IO, wbuf, 1, rbuf, 0);
  spi2_cmd(false, SPI2_CMD_CLR_USER_IO, wbuf, 10, rbuf, 0);
  spi2_cmd(false, SPI2_CMD_CLR_USER_IO, wbuf, 1, rbuf, 10);
  wbuf[0] = 0x80;
  spi2_cmd(false, SPI2_CMD_CLR_USER_IO, wbuf, 1, rbuf, 0);

  wbuf[0] = 0;
  spi2_cmd(true,  SPI2_CMD_RD_USER_IO,  wbuf, 1, rbuf, 1);
  spi2_cmd(false, SPI2_CMD_RD_USER_IO,  wbuf, 10, rbuf, 1);
  spi2_cmd(false, SPI2_CMD_RD_USER_IO,  wbuf, 1, rbuf, 10);
  wbuf[0] = 0x80;
  spi2_cmd(false, SPI2_CMD_RD_USER_IO,  wbuf, 1, rbuf, 1);

  wbuf[0] = 0;
  spi2_cmd(true,  SPI2_CMD_SET_USER_LED, wbuf, 1, rbuf, 0);
  spi2_cmd(false, SPI2_CMD_SET_USER_LED, wbuf, 10, rbuf, 0);
  spi2_cmd(false, SPI2_CMD_SET_USER_LED, wbuf, 1, rbuf, 10);
  wbuf[0] = 0x80;
  spi2_cmd(false, SPI2_CMD_SET_USER_LED, wbuf, 1, rbuf, 0);

  wbuf[0] = 0;
  spi2_cmd(true,  SPI2_CMD_CLR_USER_LED,  wbuf, 1, rbuf, 0);
  spi2_cmd(false, SPI2_CMD_CLR_USER_LED,  wbuf, 10, rbuf, 0);
  spi2_cmd(false, SPI2_CMD_CLR_USER_LED,  wbuf, 1, rbuf, 10);
  wbuf[0] = 0x80;
  spi2_cmd(false, SPI2_CMD_CLR_USER_LED,  wbuf, 1, rbuf, 0);

  wbuf[0] = 0;
  spi2_cmd(true, SPI2_CMD_RD_ITR_LOOP,  wbuf, 1, rbuf, 1);
  spi2_cmd(false, SPI2_CMD_RD_ITR_LOOP,  wbuf, 10, rbuf, 1);
  spi2_cmd(false, SPI2_CMD_RD_ITR_LOOP,  wbuf, 1, rbuf, 10);
  wbuf[0] = 0x80;
  spi2_cmd(false, SPI2_CMD_RD_ITR_LOOP,  wbuf, 1, rbuf, 1);

  wbuf[0] = 0;
  spi2_cmd(true,  SPI2_CMD_SET_SPIN_LED,  wbuf, 5, rbuf, 0);
  spi2_cmd(false, SPI2_CMD_SET_SPIN_LED,  wbuf, 10, rbuf, 0);
  spi2_cmd(false, SPI2_CMD_SET_SPIN_LED,  wbuf, 5, rbuf, 10);
  wbuf[0] = 0x80;
  spi2_cmd(false, SPI2_CMD_SET_SPIN_LED,  wbuf, 5, rbuf, 0);

  spi2_cmd(true,  SPI2_CMD_SET_EPS,  wbuf, 5, rbuf, 0);
  spi2_cmd(false, SPI2_CMD_SET_EPS,  wbuf, 10, rbuf, 0);
  spi2_cmd(false, SPI2_CMD_SET_EPS,  wbuf, 5, rbuf, 10);

  spi2_cmd(true,  SPI2_CMD_FW_VER,  wbuf, 0, rbuf, 3);
  spi2_cmd(false, SPI2_CMD_FW_VER,  wbuf, 10, rbuf, 3);
  spi2_cmd(false, SPI2_CMD_FW_VER,  wbuf, 0, rbuf, 10);

  // Bogus commands (shouldn't appear on bus)
  fprintf_P(stderr, PSTR("\nSending bogus commands...\n"));
  spi2_cmd(false, 0x00, wbuf, 0, rbuf, 0);
  spi2_cmd(false, 0xAF, wbuf, 0, rbuf, 10);
  spi2_cmd(false, 0x23, wbuf, 255, rbuf, 0);
  spi2_cmd(false, 0x7A, wbuf, 255, rbuf, 10);
  spi2_cmd(true,  0x4B, wbuf, 0, rbuf, 0);
  spi2_cmd(true,  0x5A, wbuf, 0, rbuf, 10);
  spi2_cmd(true,  0x88, wbuf, 255, rbuf, 0);
  spi2_cmd(true,  0x95, wbuf, 255, rbuf, 10);

  // Try the primary commands (0x01, 0x02, 0x04) in sequence
  fprintf_P(stderr, PSTR("\nSending commands 0x01-0x02 and 0x04 in sequence...\n"));
  spi2_cmd(false, SPI2_CMD_RST_ENC_POS, wbuf, 0, rbuf, 0);
  spi2_cmd(false, SPI2_CMD_START_TOOL_TIP, wbuf, 0, rbuf, 0);
  spi2_cmd(false, SPI2_CMD_REQ_ENC_POS, wbuf, 0, rbuf, (SPI2_NUM_AXES*4));

  // Try the remaining user commands (0x40-0x4A) in sequence
  fprintf_P(stderr, PSTR("\nSending commands 0x40-0x4A in sequence...\n"));

  // Read all the encoder axes (0x40)
  wbuf[0] = AXIS_X;
  spi2_cmd(false, SPI2_CMD_RD_ENC_POS, wbuf, 1, rbuf, 4);
  wbuf[0] = AXIS_Y;
  spi2_cmd(false, SPI2_CMD_RD_ENC_POS, wbuf, 1, rbuf, 4);
  wbuf[0] = AXIS_Z;
  spi2_cmd(false, SPI2_CMD_RD_ENC_POS, wbuf, 1, rbuf, 4);
  wbuf[0] = AXIS_A;
  spi2_cmd(false, SPI2_CMD_RD_ENC_POS, wbuf, 1, rbuf, 4);

  // Set and clear USER_IO6, reading after both steps (0x41-0x43)
  wbuf[0] = 6;
  spi2_cmd(false, SPI2_CMD_SET_USER_IO, wbuf, 1, rbuf, 0);
  spi2_cmd(false, SPI2_CMD_RD_USER_IO,  wbuf, 1, rbuf, 1);
  delay(500);
  spi2_cmd(false, SPI2_CMD_CLR_USER_IO, wbuf, 1, rbuf, 0);
  spi2_cmd(false, SPI2_CMD_RD_USER_IO,  wbuf, 1, rbuf, 1);

  // Set and clear USER_LED2 (0x44-0x45)
  wbuf[0] = 2;
  spi2_cmd(false, SPI2_CMD_SET_USER_LED, wbuf, 1, rbuf, 0);
  delay(500);
  spi2_cmd(false, SPI2_CMD_CLR_USER_LED,  wbuf, 1, rbuf, 0);

  // Read both interlock loops (0x46)
  wbuf[0] = 0;
  spi2_cmd(false, SPI2_CMD_RD_ITR_LOOP,  wbuf, 1, rbuf, 1);
  wbuf[0] = 1;
  spi2_cmd(false, SPI2_CMD_RD_ITR_LOOP,  wbuf, 1, rbuf, 1);

  // Set Spindle LEDs on LED_DOUT0 to R, G, B and W (0x47)
  wbuf[0] = 0;
  wbuf[1] = 0xFF; wbuf[2] = 0x00; wbuf[3] = 0x00; wbuf[4] = 0x00; // Red
  spi2_cmd(false, SPI2_CMD_SET_SPIN_LED,  wbuf, 5, rbuf, 0);
  delay(500);
  wbuf[1] = 0x00; wbuf[2] = 0xFF; wbuf[3] = 0x00; wbuf[4] = 0x00; // Green
  spi2_cmd(false, SPI2_CMD_SET_SPIN_LED,  wbuf, 5, rbuf, 0);
  delay(500);
  wbuf[1] = 0x00; wbuf[2] = 0x00; wbuf[3] = 0xFF; wbuf[4] = 0x00; // Blue
  spi2_cmd(false, SPI2_CMD_SET_SPIN_LED,  wbuf, 5, rbuf, 0);
  delay(500);
  wbuf[1] = 0x00; wbuf[2] = 0x00; wbuf[3] = 0x00; wbuf[4] = 0xFF; // White
  spi2_cmd(false, SPI2_CMD_SET_SPIN_LED,  wbuf, 5, rbuf, 0);
  delay(500);

  // Set each axis Epsilon to a random value (0x48)
  wbuf[0] = AXIS_X;
  wbuf[1] = 0x03;
  wbuf[2] = 0x0A;
  wbuf[3] = 0x07;
  wbuf[4] = 0x0E;
  spi2_cmd(false, SPI2_CMD_SET_EPS,  wbuf, 5, rbuf, 0);
  wbuf[0] = AXIS_Y;
  spi2_cmd(false, SPI2_CMD_SET_EPS,  wbuf, 5, rbuf, 0);
  wbuf[0] = AXIS_Z;
  spi2_cmd(false, SPI2_CMD_SET_EPS,  wbuf, 5, rbuf, 0);
  wbuf[0] = AXIS_A;
  spi2_cmd(false, SPI2_CMD_SET_EPS,  wbuf, 5, rbuf, 0);

  // Read Current (0x49) - TODO implement

  // Get Firmware Version (0x4A)
  spi2_cmd(false, SPI2_CMD_FW_VER,  wbuf, 0, rbuf, 3);

  // Random commands
  fprintf_P(stderr, PSTR("\nSending random good/bad commands...\n"));
  spi2_cmd(false, SPI2_CMD_START_TOOL_TIP, wbuf, 0, rbuf, 0);
  spi2_cmd(false, SPI2_CMD_RST_ENC_POS, wbuf, 0, rbuf, 0);
  wbuf[0] = 0;
  wbuf[1] = 0x00; wbuf[2] = 0xFF; wbuf[3] = 0x00; wbuf[4] = 0x00; // Green
  spi2_cmd(false, SPI2_CMD_SET_SPIN_LED,  wbuf, 5, rbuf, 0);
  delay(500);
  spi2_cmd(false, SPI2_CMD_RST_ENC_POS, wbuf, 0, rbuf, 0);
  spi2_cmd(false, 0x00, wbuf, 0, rbuf, 0);
  wbuf[0] = 2;
  spi2_cmd(false, SPI2_CMD_SET_USER_LED, wbuf, 1, rbuf, 0);
  spi2_cmd(false, SPI2_CMD_REQ_ENC_POS, wbuf, 0, rbuf, (SPI2_NUM_AXES*4));
  wbuf[0] = AXIS_Z;
  wbuf[1] = 0x03;
  wbuf[2] = 0x0A;
  wbuf[3] = 0x07;
  wbuf[4] = 0x0E;
  spi2_cmd(false, SPI2_CMD_SET_EPS,  wbuf, 5, rbuf, 0);
  spi2_cmd(false, SPI2_CMD_RST_ENC_POS, wbuf, 0, rbuf, 0);
  wbuf[0] = AXIS_Z;
  spi2_cmd(false, SPI2_CMD_RD_ENC_POS, wbuf, 1, rbuf, 4);

  // Additional Manual Tests:
  // Disconnect MOSI, MISO to test retry 3x and timeouts
  // Command 0x03 triggered by SPI2 slave interrupt request

  fprintf_P(stderr, PSTR("\n*** SPI2 Test END ***\n"));
}

// SPI2 Slave ISR
void Pin<kSocket3_SPISlaveSelectPinNumber>::interrupt() {

  // Set Slave Interrupt Flag
  spi2_slave_int = true;

}


/////////////////////////////////////////////////////////////
// Begin SW commands (mostly wrappers for SPI2 functions)
/////////////////////////////////////////////////////////////

//
// Commands:
//
//  cmd1  - Reset Encoder Positions to Zero
//  cmd2  - Start Tool Tip Command
//  cmd4  - Request Encoder Positions
//  cmd40 - Read Encoder Position
//  cmd41 - Set User IO
//  cmd42 - Clear User IO
//  cmd43 - Read User IO
//  cmd44 - Set User LED
//  cmd45 - Clear User LED
//  cmd46 - Read Interlock Loop
//  cmd47 - Set Spindle LED
//  cmd48 - Set Epsilon
//  cmd49 - Read ESC Current (Skipped)
//  cmd4a - Firmware Version
//

// spi2_cmd_helper: helper function to return proper status
stat_t spi2_cmd_helper(uint8_t sts_byte) {

  stat_t status;

  // Convert status byte to TinyG status code
  // (0x00 over SPI2 not in use, potential false OK status)
  switch (sts_byte) {
    case SPI2_STS_OK:       status = STAT_OK; break;
    case SPI2_STS_TIMEOUT:  status = STAT_EAGAIN; break;
    case SPI2_STS_RETRIES:  status = STAT_EAGAIN; break;
    default:                status = STAT_ERROR; break;
  }

  return status;
}

stat_t spi2_cmd64_get(nvObj_t *nv) {

  // Get the variable based on the current encoder index
  switch (spi2_enc_idx) {
    case AXIS_X: nv->value = *((float *)&spi2_encoder_pos[AXIS_X]); break;
    case AXIS_Y: nv->value = *((float *)&spi2_encoder_pos[AXIS_Y]); break;
    case AXIS_Z: nv->value = *((float *)&spi2_encoder_pos[AXIS_Z]); break;
    case AXIS_A: nv->value = *((float *)&spi2_encoder_pos[AXIS_A]); break;
    default: nv->value = 0.0; break;
  }

	nv->precision = (int8_t)GET_TABLE_WORD(precision);
	nv->valuetype = TYPE_FLOAT;

  return STAT_OK;
}

stat_t spi2_cmd1_set(nvObj_t *nv) {
  return (spi2_cmd_helper(spi2_reset_encoder_positions()));
}

stat_t spi2_cmd2_set(nvObj_t *nv) {
	return (spi2_cmd_helper(spi2_start_tool_tip()));
}

stat_t spi2_cmd4_set(nvObj_t *nv) {
  return (spi2_cmd_helper(spi2_request_encoder_positions()));
}

stat_t spi2_cmd64_set(nvObj_t *nv) {
  return (spi2_cmd_helper(spi2_read_encoder_position((uint8_t)nv->value)));
}

stat_t spi2_cmd65_set(nvObj_t *nv) {
  return (spi2_cmd_helper(spi2_set_user_io((uint8_t)nv->value)));
}

stat_t spi2_cmd66_set(nvObj_t *nv) {
  return (spi2_cmd_helper(spi2_clear_user_io((uint8_t)nv->value)));
}

stat_t spi2_cmd67_set(nvObj_t *nv) {
  return (spi2_cmd_helper(spi2_read_user_io((uint8_t)nv->value)));
}

stat_t spi2_cmd68_set(nvObj_t *nv) {
  return (spi2_cmd_helper(spi2_set_user_led((uint8_t)nv->value)));
}

stat_t spi2_cmd69_set(nvObj_t *nv) {
  return (spi2_cmd_helper(spi2_clear_user_led((uint8_t)nv->value)));
}

stat_t spi2_cmd70_set(nvObj_t *nv) {
  return (spi2_cmd_helper(spi2_read_itr_loop((uint8_t)nv->value)));
}

stat_t spi2_cmd71_set(nvObj_t *nv) {

  // Call set_grp() to set all the variables using the JSON
  set_grp(nv);

  // Run the SPI command
  return (spi2_cmd_helper(spi2_set_spindle_led()));
}

stat_t spi2_cmd72_set(nvObj_t *nv) {

  // Call set_grp() to set all the variables using the JSON
  set_grp(nv);

  // Run the SPI command
  return (spi2_cmd_helper(spi2_set_epsilon()));
}

stat_t spi2_cmd74_set(nvObj_t *nv) {
  return (spi2_cmd_helper(spi2_get_fw_version()));
}

// Print functions (text-mode only)
#ifdef __TEXT_MODE

static const char msg_units0[] PROGMEM = " in";	// used by generic print functions
static const char msg_units1[] PROGMEM = " mm";
static const char msg_units2[] PROGMEM = " deg";
static const char *const msg_units[] PROGMEM = { msg_units0, msg_units1, msg_units2 };

static const char fw_ver0[] PROGMEM = "MAJOR Version";
static const char fw_ver1[] PROGMEM = "MINOR Version";
static const char fw_ver2[] PROGMEM = "REVISION";
static const char *const fw_vers[] PROGMEM = { fw_ver0, fw_ver1, fw_ver2 };

static const char fmt_spi2_cmd1[] PROGMEM  = "Reset Encoder Positions to Zero\n";
static const char fmt_spi2_cmd2[] PROGMEM  = "Start Tool Tip Command\n";
static const char fmt_spi2_cmd4[] PROGMEM  = "%c Encoder Position:%15.3f%s\n";
static const char fmt_spi2_cmd64[] PROGMEM = "%c Encoder Position:%15.3f%s\n";
static const char fmt_spi2_cmd65[] PROGMEM = "Set User IO %u\n";
static const char fmt_spi2_cmd66[] PROGMEM = "Clear User IO %u\n";
static const char fmt_spi2_cmd67[] PROGMEM = "User IO %u value: %u\n";
static const char fmt_spi2_cmd68[] PROGMEM = "Set User LED %u\n";
static const char fmt_spi2_cmd69[] PROGMEM = "Clear User LED %u\n";
static const char fmt_spi2_cmd70[] PROGMEM = "Interlock Loop %u value: %u\n";
static const char fmt_spi2_cmd71[] PROGMEM = "Spindle LED %s: %X\n";
static const char fmt_spi2_cmd74[] PROGMEM = "Firmware %s Number: %u\n";

static int8_t _get_axis(const index_t index)
{
	char_t *ptr;
	char_t tmp[TOKEN_LEN+1];
	char_t axes[] = {"xyza"};

	strncpy_P(tmp, cfgArray[index].token, TOKEN_LEN);	// kind of a hack. Looks for an axis
	if ((ptr = strchr(axes, tmp[0])) == NULL) {			// character in the 0 and 3 positions
		if ((ptr = strchr(axes, tmp[3])) == NULL) {		// to accommodate 'xam' and 'g54x' styles
			return (-1);
		}
	}
	return (ptr - axes);
}

static int8_t _get_fw_ver(const index_t index)
{
	char_t *ptr;
	char_t tmp[TOKEN_LEN+1];
	char_t fw_ver_tokens[] = {"air"};

	strncpy_P(tmp, cfgArray[index].token, TOKEN_LEN);	// kind of a hack. Looks for a version type
	if ((ptr = strchr(fw_ver_tokens, tmp[4])) == NULL) {
	   return -1;
	}
	return (ptr - fw_ver_tokens);
}

static void _print_all_enc_pos(nvObj_t *nv, const char *format, uint8_t units)
{
	char axes[] = {"XYZA"};
	uint8_t axis = _get_axis(nv->index);
	fprintf_P(stderr, format, axes[axis], nv->value, GET_TEXT_ITEM(msg_units, units));
}

static void _print_single_enc_pos(nvObj_t *nv, const char *format, uint8_t units)
{
	char enc_axis = 0;
  float enc_value = 0.0;

  // Set axis and value based on current index
  switch (spi2_enc_idx) {
    case AXIS_X: enc_axis = 'X'; enc_value = spi2_encoder_pos[AXIS_X]; break;
    case AXIS_Y: enc_axis = 'Y'; enc_value = spi2_encoder_pos[AXIS_Y]; break;
    case AXIS_Z: enc_axis = 'Z'; enc_value = spi2_encoder_pos[AXIS_Z]; break;
    case AXIS_A: enc_axis = 'A'; enc_value = spi2_encoder_pos[AXIS_A]; break;
    default: enc_axis = '?'; enc_value = 0.0; break;
  }

	fprintf_P(stderr, format, enc_axis, enc_value, GET_TEXT_ITEM(msg_units, units));
}

static void _print_user_io(nvObj_t *nv, const char *format)
{
  fprintf_P(stderr, format, spi2_io_idx, spi2_io_val);
}

static void _print_interlock(nvObj_t *nv, const char *format)
{
  fprintf_P(stderr, format, spi2_itr_idx, spi2_itr_val);
}

static void _print_fw_version(nvObj_t *nv, const char *format)
{
  uint8_t fw_ver_idx = _get_fw_ver(nv->index);
  fprintf_P(stderr, format, GET_TEXT_ITEM(fw_vers, fw_ver_idx), (uint8_t)nv->value);
}

void spi2_cmd1_print(nvObj_t *nv) { text_print_nul(nv, fmt_spi2_cmd1);}
void spi2_cmd2_print(nvObj_t *nv) { text_print_nul(nv, fmt_spi2_cmd2);}
void spi2_cmd4_print(nvObj_t *nv) { _print_all_enc_pos(nv, fmt_spi2_cmd4, cm_get_units_mode(MODEL));}
void spi2_cmd64_print(nvObj_t *nv) { _print_single_enc_pos(nv, fmt_spi2_cmd64, cm_get_units_mode(MODEL));}
void spi2_cmd65_print(nvObj_t *nv) { text_print_ui8(nv, fmt_spi2_cmd65);}
void spi2_cmd66_print(nvObj_t *nv) { text_print_ui8(nv, fmt_spi2_cmd66);}
void spi2_cmd67_print(nvObj_t *nv) { _print_user_io(nv, fmt_spi2_cmd67);}
void spi2_cmd68_print(nvObj_t *nv) { text_print_ui8(nv, fmt_spi2_cmd68);}
void spi2_cmd69_print(nvObj_t *nv) { text_print_ui8(nv, fmt_spi2_cmd69);}
void spi2_cmd70_print(nvObj_t *nv) { _print_interlock(nv, fmt_spi2_cmd70);}
void spi2_cmd74_print(nvObj_t *nv) { _print_fw_version(nv, fmt_spi2_cmd74);}
#endif

/////////////////////////////////////////////////////////////
// End SW commands
/////////////////////////////////////////////////////////////
