HDF5 "tslink.h5" {
SOFTLINK "/slink1" {
   LINKTARGET "somevalue"
}
}
HDF5-DIAG: Error detected in HDF5 (version (number)) thread (IDs):
  #000: (file name) line (number) in H5Dopen2(): not found
    major: Dataset
    minor: Object not found
  #001: (file name) line (number) in H5G_loc_find(): can't find object
    major: Symbol table
    minor: Object not found
  #002: (file name) line (number) in H5G_traverse(): internal path traversal failed
    major: Symbol table
    minor: Object not found
  #003: (file name) line (number) in H5G_traverse_real(): special link traversal failed
    major: Links
    minor: Link traversal failure
  #004: (file name) line (number) in H5G__traverse_special(): symbolic link traversal failed
    major: Links
    minor: Link traversal failure
  #005: (file name) line (number) in H5G_traverse_slink(): unable to follow symbolic link
    major: Symbol table
    minor: Object not found
  #006: (file name) line (number) in H5G_traverse_real(): traversal operator failed
    major: Symbol table
    minor: Callback failed
  #007: (file name) line (number) in H5G_traverse_slink_cb(): component not found
    major: Symbol table
    minor: Object not found
