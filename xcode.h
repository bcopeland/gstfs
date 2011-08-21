#ifndef _XCODE_H
#define _XCODE_H

extern int transcode(char *pipeline, char *filename, 
    int (*add_data_cb)(char *, size_t, void *), void *user_data);

#endif /* _XCODE_H */
