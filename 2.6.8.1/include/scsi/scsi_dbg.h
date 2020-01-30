#ifndef _SCSI_SCSI_DBG_H
#define _SCSI_SCSI_DBG_H

struct scsi_cmnd;
struct scsi_request;

extern void scsi_print_command(struct scsi_cmnd *);
extern void __scsi_print_command(unsigned char *);
extern void scsi_print_sense(const char *, struct scsi_cmnd *);
extern void scsi_print_req_sense(const char *, struct scsi_request *);
extern void scsi_print_driverbyte(int);
extern void scsi_print_hostbyte(int);
extern void scsi_print_status(unsigned char);
extern int scsi_print_msg(const unsigned char *);
extern const char *scsi_sense_key_string(unsigned char);
extern const char *scsi_extd_sense_format(unsigned char, unsigned char);

#endif /* _SCSI_SCSI_DBG_H */
