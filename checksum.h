#ifndef CHECKSUM_H
#define CHECKSUM_H

int checksum_verify_file(const char* path, const char* spec,
                         char* actual_hex, int actual_n,
                         char* err, int err_n);

#endif /* CHECKSUM_H */
