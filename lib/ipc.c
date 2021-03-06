#include<libtransistor/ipc.h>
#include<libtransistor/tls.h>
#include<libtransistor/svc.h>
#include<libtransistor/err.h>
#include<libtransistor/util.h>

result_t ipc_marshal(u32 *buffer, ipc_request_t *rq) {
  int h = 0; // h is for HEAD
  
  ipc_buffer_t *a_descriptors[16];
  ipc_buffer_t *b_descriptors[16];
  ipc_buffer_t *c_descriptors[16];
  ipc_buffer_t *x_descriptors[16];
  
  int num_a_descriptors = 0, num_b_descriptors = 0,
    num_c_descriptors = 0, num_x_descriptors = 0;

  // group buffers by descriptor type
  for(int i = 0; i < rq->num_buffers; i++) {
    ipc_buffer_t *buffer = rq->buffers[i];
    if(!(buffer->type & 0x20)) {
      int direction = (buffer->type & 0b0011) >> 0; // in or out (ax or bc)
      int family    = (buffer->type & 0b1100) >> 2; // ab or xc

      ipc_buffer_t **list;
      int *count;
      
      if(direction == 0b01) { // IN (ax)
        if(family == 0b01) { // A
          list = a_descriptors;
          count = &num_a_descriptors;
        } else if(family == 0b10) { // X
          list = x_descriptors;
          count = &num_x_descriptors;
        } else {
          return LIBTRANSISTOR_ERR_UNSUPPORTED_BUFFER_TYPE;
        }
      } else if(direction == 0b10) { // OUT (bc)
        if(family == 0b01) { // B
          list = b_descriptors;
          count = &num_b_descriptors;
        } else if(family == 0b10) { // C
          list = c_descriptors;
          count = &num_c_descriptors;
        } else {
          return LIBTRANSISTOR_ERR_UNSUPPORTED_BUFFER_TYPE;
        }
      } else {
        return LIBTRANSISTOR_ERR_UNSUPPORTED_BUFFER_TYPE;
      }

      // make sure we don't overflow our descriptor count fields
      if(*count >= 16) {
        return LIBTRANSISTOR_ERR_TOO_MANY_BUFFERS;
      }

      // add the buffer to the list
      list[(*count)++] = buffer;
    } else { // flag 0x20 is complicated
      return LIBTRANSISTOR_ERR_UNSUPPORTED_BUFFER_TYPE;
    }
  }

  // type must fit within 16 bits
  if(rq->type & ~0xFFFF) {
    return LIBTRANSISTOR_ERR_INVALID_REQUEST_TYPE;
  }

  // header field 1
  buffer[h++] = rq->type
    | (num_x_descriptors << 16)
    | (num_a_descriptors << 20)
    | (num_b_descriptors << 24)
    | (0 << 28); // "w" descriptors

  int c_descriptor_flags = 0;
  if(num_c_descriptors == 1) {
    c_descriptor_flags = 2;
  } else if(num_c_descriptors > 1) {
    c_descriptor_flags = num_c_descriptors + 2;
  }

  int raw_data_section_size = rq->raw_data_size + 4 + 4;
  int handle_descriptor_enabled = rq->num_copy_handles || rq->num_move_handles || rq->send_pid;

  // header field 2
  buffer[h++] = raw_data_section_size
    | (c_descriptor_flags << 10)
    | (handle_descriptor_enabled << 31);

  // handle descriptor
  if(handle_descriptor_enabled) {
    if(rq->num_copy_handles >= 16 || rq->num_move_handles >= 16) {
      return LIBTRANSISTOR_ERR_TOO_MANY_HANDLES;
    }
    
    buffer[h++] = (rq->send_pid ? 1 : 0)
      | (rq->num_copy_handles << 1)
      | (rq->num_move_handles << 5);

    h+= (rq->send_pid ? 2 : 0);

    for(int i = 0; i < rq->num_copy_handles; i++) {
      buffer[h++] = rq->copy_handles[i];
    }
    for(int i = 0; i < rq->num_move_handles; i++) {
      buffer[h++] = rq->move_handles[i];
    }
  }

  // x descriptors
  for(int i = 0; i < num_x_descriptors; i++) {
    int counter = i;
    ipc_buffer_t *buf = x_descriptors[i];

    if((u64) buf->addr >> 38) {
      return LIBTRANSISTOR_ERR_INVALID_BUFFER_ADDRESS;
    }

    if(buf->size >> 16) {
      return LIBTRANSISTOR_ERR_INVALID_BUFFER_SIZE;
    }
    
    buffer[h++] = counter
      | (((u64) buf->addr >> 36) & 0b111) << 6
      | ((counter >> 9) & 0b111) << 9
      | (((u64) buf->addr >> 32) & 0b1111) << 12
      | (buf->size << 16);

    buffer[h++] = (u64) buf->addr & 0xFFFFFFFF;
  }

  // a & b descriptors
  for(int i = 0; i < num_a_descriptors + num_b_descriptors; i++) {
    ipc_buffer_t *buf = ((i < num_a_descriptors) ? a_descriptors : b_descriptors)[i];

    if((u64) buf->addr >> 38) {
      return LIBTRANSISTOR_ERR_INVALID_BUFFER_ADDRESS;
    }

    if(buf->size >> 35) {
      return LIBTRANSISTOR_ERR_INVALID_BUFFER_SIZE;
    }

    buffer[h++] = buf->size & 0xFFFFFFFF;
    buffer[h++] = (u64) buf->addr & 0xFFFFFFFF;

    if(buf->type >> 6) {
      return LIBTRANSISTOR_ERR_INVALID_PROTECTION;
    }
    
    buffer[h++] = (buf->type >> 6) // flags/permissions
      | (((u64) buf->addr >> 36) & 0b111) << 2
      | ((buf->size >> 32) & 0b1111) << 24
      | (((u64) buf->addr >> 32) & 0b1111) << 28;
  }
  
  // "w" descriptors would go here

  // raw data
  // align head to 4 words
  int raw_data_start = h;
  h = (h + 3) & ~3;
  int pre_padding = h - raw_data_start; // the padding before this section and after it needs to add up to be 0x10 bytes long

  buffer[h++] = *((uint32_t*) "SFCI");
  buffer[h++] = 0;
  buffer[h++] = rq->request_id;
  buffer[h++] = 0;
  
  for(int i = 0; i < rq->raw_data_size; i++) {
    buffer[h++] = rq->raw_data[i];
  }

  h+= 0x10 - pre_padding;

  int u16_length_count;
  uint16_t *u16_length_list = (uint16_t*) (buffer + h);
  
  // c descriptor u16 length list
  for(int i = 0; i < num_c_descriptors; i++) {
    ipc_buffer_t *buf = c_descriptors[i];
    if(buf->type & 0x10) { // u16 length list flag
      if(buf->size >> 16) {
        return LIBTRANSISTOR_ERR_INVALID_BUFFER_SIZE;
      }
      u16_length_list[u16_length_count++] = buf->size;
    }
  }
  h+= (u16_length_count + 1) >> 1;
  
  // c descriptors
  for(int i = 0; i < num_c_descriptors; i++) {
    ipc_buffer_t *buf = c_descriptors[i];

    if((u64) buf->addr >> 48) {
      return LIBTRANSISTOR_ERR_INVALID_BUFFER_ADDRESS;
    }

    if(buf->size >> 16) {
      return LIBTRANSISTOR_ERR_INVALID_BUFFER_SIZE;
    }
    
    buffer[h++] = (u64) buf->addr & 0xFFFFFFFF;
    buffer[h++] = ((u64) buf->addr >> 32)
      | (buf->size << 16);
  }

  return RESULT_OK;
}

result_t ipc_unmarshal(u32 *buffer, ipc_response_fmt_t *rs) {
  int h = 0; // h for HEAD

  u32 header0 = buffer[h++];
  u32 header1 = buffer[h++];
  int response_type = header0 & 0xFFFF;

  if(response_type != 0) {
    return LIBTRANSISTOR_ERR_INVALID_IPC_RESPONSE_TYPE;
  }
  
  int num_x_descriptors = (header0 >> 16) & 0xF;
  int num_a_descriptors = (header0 >> 20) & 0xF;
  int num_b_descriptors = (header0 >> 24) & 0xF;
  int num_w_descriptors = (header0 >> 28) & 0xF;

  int raw_data_section_size = header1 & 0b1111111111;
  
  int c_descriptor_flags = (header1 >> 10) & 0xF;
  bool has_handle_descriptor = header1 >> 31;

  int num_copy_handles = 0;
  int num_move_handles = 0;
  handle_t *copy_handles;
  handle_t *move_handles;

  bool has_pid = false;
  int pid;
  
  if(has_handle_descriptor) {
    int handle_descriptor = buffer[h++];
    if(handle_descriptor & 1) {
      has_pid = true;
      pid = *(u64*)(buffer + h);
      h+= 2;
    }
    num_copy_handles = (handle_descriptor >> 1) & 0xF;
    num_move_handles = (handle_descriptor >> 5) & 0xF;
    copy_handles = buffer + h; h+= num_copy_handles;
    move_handles = buffer + h; h+= num_move_handles;
  }

  // skip descriptors
  h+= num_x_descriptors * 2;
  h+= num_a_descriptors * 3;
  h+= num_b_descriptors * 3;
  h+= num_w_descriptors * 3;

  // align head to 4 words
  int raw_data_start = h;
  h = (h + 3) & ~3;

  if(buffer[h++] != *((uint32_t*) "SFCO")) {
    return LIBTRANSISTOR_ERR_INVALID_IPC_RESPONSE_MAGIC;
  }
  h++;

  // if this isn't ok, none of our other expectations will make
  // sense, so this is the most meaningful result to return.
  result_t response_code = buffer[h++];
  if(response_code != RESULT_OK) {
    return response_code;
  }
  h++;

  u32 *raw_data = buffer + h;
  
  if((raw_data_section_size - 8) != rs->raw_data_size) {
    hexnum(raw_data_section_size);
    printf("raw data size doesn't match (0x%x != 0x%x)", raw_data_section_size - 8, rs->raw_data_size);
    return LIBTRANSISTOR_ERR_UNEXPECTED_RAW_DATA_SIZE;
  }
  
  if(has_pid != rs->has_pid) {
    return LIBTRANSISTOR_ERR_UNEXPECTED_PID;
  }

  if(num_copy_handles != rs->num_copy_handles) {
    return LIBTRANSISTOR_ERR_UNEXPECTED_COPY_HANDLES;
  }

  if(num_move_handles != rs->num_move_handles) {
    return LIBTRANSISTOR_ERR_UNEXPECTED_MOVE_HANDLES;
  }
  
  for(int i = 0; i < rs->num_copy_handles; i++) { rs->copy_handles[i] = copy_handles[i]; }
  for(int i = 0; i < rs->num_move_handles; i++) { rs->move_handles[i] = move_handles[i]; }
  for(int i = 0; i < rs->raw_data_size; i++) {
    rs->raw_data[i] = raw_data[i];
  }
  
  return RESULT_OK;
}

result_t ipc_send(session_h session, ipc_request_t *rq, ipc_response_fmt_t *rs) {
  result_t r;
  u32 *tls = get_tls();
  r = ipc_marshal(tls, rq); if(r) { return r; }
  hexdump(tls, 0x40);  
  r = svcSendSyncRequest(session); if(r) { return r; }
  hexdump(tls, 0x40);
  r = ipc_unmarshal(tls, rs); if(r) { return r; }

  return RESULT_OK;
}
