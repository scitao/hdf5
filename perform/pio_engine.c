/*
 * Copyright (C) 2001
 *     National Center for Supercomputing Applications
 *     All rights reserved.
 * 
 * Author: Albert Cheng of NCSA, Oct 24, 2001.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include "hdf5.h"

#ifdef H5_HAVE_PARALLEL

#include <mpi.h>

#ifndef MPI_FILE_NULL           /*MPIO may be defined in mpi.h already       */
#   include <mpio.h>
#endif  /* !MPI_FILE_NULL */

#include "pio_perf.h"
#include "pio_timer.h"

/* Macro definitions */

/* sizes of various items. these sizes won't change during program execution */
#define ELMT_SIZE           sizeof(int)                 /* we're doing ints */

#define GOTOERROR(errcode)	{ ret_code = errcode; goto done; }
#define GOTODONE		{ goto done; }
#define ERRMSG(mesg) {                                                  \
    fprintf(stderr, "Proc %d: ", myrank);                               \
    fprintf(stderr, "*** Assertion failed (%s) at line %4d in %s\n",    \
            mesg, (int)__LINE__, __FILE__);                             \
}

#define MSG(mesg) {                                     \
    fprintf(stderr, "Proc %d: ", myrank);               \
    fprintf(stderr, "(%s) at line %4d in %s\n",         \
            mesg, (int)__LINE__, __FILE__);             \
}

/* verify: if val is false (0), print mesg. */
#define VRFY(val, mesg) do {                            \
    if (!val) {                                         \
        ERRMSG(mesg);                                   \
        GOTOERROR(FAIL);                                \
    }                                                   \
} while(0)

#ifndef HDopen
#  ifdef O_BINARY
#    define HDopen(S,F,M)       open(S,F|_O_BINARY,M)
#  else   /* O_BINARY */
#    define HDopen(S,F,M)       open(S,F,M)
#  endif  /* !O_BINARY */
#endif  /* !HDopen */

#ifndef HDclose
#  define HDclose(F)            close(F)
#endif  /* !HDclose */

#ifndef HDseek
#  define HDseek(F,L,W)         lseek(F,L,W)
#endif  /* !HDseek */

#ifndef HDwrite
#  define HDwrite(F,B,S)        write(F,B,S)
#endif  /* !HDwrite */

#ifndef HDread
#  define HDread(F,B,S)         read(F,B,S)
#endif  /* !HDread */

/* Raw I/O macros */
#define RAWCREATE(fn)           HDopen(fn, O_CREAT|O_TRUNC|O_RDWR, 0600)
#define RAWOPEN(fn, F)          HDopen(fn, F, 0600)
#define RAWCLOSE(F)             HDclose(F)
#define RAWSEEK(F,L)            HDseek(F,(off_t) L,SEEK_SET)
#define RAWWRITE(F,B,S)         HDwrite(F,B,S)
#define RAWREAD(F,B,S)          HDread(F,B,S)

enum {
    PIO_CREATE = 1,
    PIO_WRITE = 2,
    PIO_READ = 4
};

/*
 * In a parallel machine, the filesystem suitable for compiling is
 * unlikely a parallel file system that is suitable for parallel I/O.
 * There is no standard pathname for the parallel file system.  /tmp
 * is about the best guess.
 */
#ifndef HDF5_PARAPREFIX
#  ifdef __PUMAGON__
     /* For the PFS of TFLOPS */
#    define HDF5_PARAPREFIX     "pfs:/pfs_grande/multi/tmp_1"
#  else
#    define HDF5_PARAPREFIX     "/tmp"
#  endif    /* __PUMAGON__ */
#endif  /* !HDF5_PARAPREFIX */

#ifndef MIN
#define MIN(a,b) (a < b ? a : b)
#endif  /* !MIN */

/* the different types of file descriptors we can expect */
typedef union _file_descr {
    int         rawfd;      /* raw/Unix file    */
    MPI_File    mpifd;      /* MPI file         */
    hid_t       h5fd;       /* HDF5 file        */
} file_descr;

/* local functions */
static char  *pio_create_filename(iotype iot, const char *base_name,
                                  char *fullname, size_t size);
static herr_t do_write(file_descr *fd, iotype iot, long ndsets,
                       long nelmts, long buf_size, char *buffer, MPI_Comm comm);
static herr_t do_fopen(iotype iot, char *fname, file_descr *fd /*out*/,
                       int flags, MPI_Comm comm);
static herr_t do_fclose(iotype iot, file_descr *fd);

results
do_pio(parameters param)
{
    /* return codes */
    int		rc;             /*routine return code                   */
    int         mrc;            /*MPI return code                       */
    herr_t      ret_code = 0;   /*return code                           */
    results     res;

    file_descr	fd;
    iotype      iot;

    char        fname[FILENAME_MAX];
    int         maxprocs, nfiles, nf;
    long        ndsets, nelmts;
    int         niters;
    int         color;                  /*for communicator creation     */
    char        *buffer = NULL;         /*data buffer pointer           */
    long	buf_size;		/*data buffer size in bytes     */

    /* HDF5 variables */
    herr_t          hrc;                    /*HDF5 return code              */

    /* MPI variables */
    MPI_Comm	    comm = MPI_COMM_NULL;
    int             myrank, nprocs = 1;

    /* Sanity check parameters */

    /* IO type */
    iot = param.io_type;

    switch (iot) {
    case MPIO:
	fd.mpifd = MPI_FILE_NULL;
        res.timers = pio_time_new(MPI_TIMER);
        break;
    case RAW:
	fd.rawfd = -1;
        res.timers = pio_time_new(SYS_TIMER);
	break;
    case PHDF5:
	fd.h5fd = -1;
        res.timers = pio_time_new(SYS_TIMER);
        break;

    default:
        /* unknown request */
        fprintf(stderr, "Unknown IO type request (%d)\n", iot);
        GOTOERROR(FAIL);
    }

    nfiles = param.num_files;       /* number of files                      */
    ndsets = param.num_dsets;       /* number of datasets per file          */
    nelmts = param.num_elmts;       /* number of elements per dataset       */
    niters = param.num_iters;       /* number of iterations of reads/writes */
    maxprocs = param.max_num_procs; /* max number of mpi-processes to use   */
    buf_size = param.buf_size;

    if (nelmts <= 0 ) {
        fprintf(stderr,
                "number of elements per dataset must be > 0 (%ld)\n",
                nelmts);
        GOTOERROR(FAIL);
    }

    if (maxprocs <= 0 ) {
        fprintf(stderr,
                "maximum number of process to use must be > 0 (%d)\n",
                maxprocs);
        GOTOERROR(FAIL);
    }

    MPI_Comm_size(MPI_COMM_WORLD, &nprocs);

    if (maxprocs > nprocs) {
        fprintf(stderr,
                "maximum number of process(%d) must be <= process in MPI_COMM_WORLD(%d)\n",
                maxprocs, nprocs);
        GOTOERROR(FAIL);
    }

    if (buf_size <= 0 ){
        fprintf(stderr,
                "buffer size must be > 0 (%ld)\n", buf_size);
        GOTOERROR(FAIL);
    }

/* DEBUG*/
fprintf(stderr, "nfiles=%d\n", nfiles);
fprintf(stderr, "ndsets=%ld\n", ndsets);
fprintf(stderr, "nelmts=%ld\n", nelmts);
fprintf(stderr, "niters=%d\n", niters);
fprintf(stderr, "maxprocs=%d\n", maxprocs);
fprintf(stderr, "buffer size=%ld\n", buf_size);
/* nfiles=3; */
/*ndsets=5; */

    /* Create a sub communicator for this run. Easier to use the first N
     * processes. */
    MPI_Comm_rank(MPI_COMM_WORLD, &myrank);
    color = (myrank < maxprocs);
    mrc = MPI_Comm_split(MPI_COMM_WORLD, color, myrank, &comm);

    if (mrc != MPI_SUCCESS) {
        fprintf(stderr, "MPI_Comm_split failed\n");
        GOTOERROR(FAIL);
    }

    if (!color){
        /* not involved in this run */
        mrc = MPI_Comm_free(&comm);
        GOTODONE;
    }

    /* determine the mpi rank of in the new comm */
    MPI_Comm_size(comm, &nprocs);
    MPI_Comm_rank(comm, &myrank);

    /* allocate data buffer */
    buffer = malloc(buf_size);

    if (buffer == NULL){
        fprintf(stderr, "malloc for data buffer size (%ld) failed\n",
	    buf_size);
        GOTOERROR(FAIL);
    }

    for (nf = 1; nf <= nfiles; nf++) {
	/*
	 * Wirte performance measurement
	 */
        /* Open file for write */
        char base_name[256];

        sprintf(base_name, "#pio_tmp_%u", nf);
        pio_create_filename(iot, base_name, fname, sizeof(fname));
fprintf(stderr, "filename=%s\n", base_name);

        set_time(res.timers, HDF5_FILE_OPENCLOSE, START);
        hrc = do_fopen(iot, fname, &fd, PIO_CREATE | PIO_WRITE, comm);
        set_time(res.timers, HDF5_FILE_OPENCLOSE, STOP);

        VRFY((hrc == SUCCESS), "do_fopen failed");

        set_time(res.timers, HDF5_WRITE_FIXED_DIMS, START);
        hrc = do_write(&fd, iot, ndsets, nelmts, buf_size, buffer, comm);
        set_time(res.timers, HDF5_WRITE_FIXED_DIMS, STOP);

        VRFY((hrc == SUCCESS), "do_write failed");

        /* Close file for write */
        set_time(res.timers, HDF5_FILE_OPENCLOSE, START);
        hrc = do_fclose(iot, &fd);
        set_time(res.timers, HDF5_FILE_OPENCLOSE, STOP);

        VRFY((hrc == SUCCESS), "do_fclose failed");

	/*
	 * Read performance measurement
	 */
        /* Open file for read */
        hrc = do_fopen(iot, fname, &fd, PIO_READ, comm);
        VRFY((hrc == SUCCESS), "do_fopen failed");

	/*
	hrc = do_read(&fd, iot, ndsets, nelmts, buffer);
	        VRFY((hrc == SUCCESS), "do_read failed");
	*/


        /* Close file for read */
        hrc = do_fclose(iot, &fd);
        VRFY((hrc == SUCCESS), "do_fclose failed");
        remove(fname);
    }

done:
    /* clean up */
    /* release HDF5 objects */

    /* close any opened files */
    /* no remove(fname) because that should have happened normally. */
    switch (iot) {
    case RAW:
	if (fd.rawfd != -1)
	    hrc = do_fclose(iot, &fd);
        break;
    case MPIO:
        if (fd.mpifd != MPI_FILE_NULL)
	    hrc = do_fclose(iot, &fd);
        break;
    case PHDF5:
        if (fd.h5fd != -1)
	    hrc = do_fclose(iot, &fd);
        break;
    }

    /* release MPI resources */
    if (comm != MPI_COMM_NULL){
        mrc = MPI_Comm_free(&comm);
	if (mrc != MPI_SUCCESS) {
	    fprintf(stderr, "MPI_Comm_free failed\n");
	    ret_code = FAIL;
	}
    }

    /* release generic resources */
    free(buffer);
    res.ret_code = ret_code;
    return res;
}

/*
 * Function:    pio_create_filename
 * Purpose:     Create a new filename to write to. Determine the correct
 *              suffix to append to the filename by the type of I/O we're
 *              doing. Also, place in the /tmp/{$USER,$LOGIN} directory if
 *              USER or LOGIN are specified in the environment.
 * Return:      Pointer to filename or NULL
 * Programmer:  Bill Wendling, 21. November 2001
 * Modifications:
 */
static char *
pio_create_filename(iotype iot, const char *base_name, char *fullname, size_t size)
{
    const char *prefix, *suffix;
    char *ptr, last = '\0';
    size_t i, j;

    if (!base_name || !fullname || size < 1)
        return NULL;

    memset(fullname, 0, size);

    switch (iot) {
    case RAW:
        suffix = ".raw";
        break;
    case MPIO:
        suffix = ".mpio";
        break;
    case PHDF5:
        suffix = ".h5";
        break;
    }

    /* First use the environment variable and then try the constant */
    prefix = getenv("HDF5_PARAPREFIX");

#ifdef HDF5_PARAPREFIX
    if (!prefix)
        prefix = HDF5_PARAPREFIX;
#endif  /* HDF5_PARAPREFIX */

    /* Prepend the prefix value to the base name */
    if (prefix && *prefix) {
        /* If the prefix specifies the HDF5_PARAPREFIX directory, then
         * default to using the "/tmp/$USER" or "/tmp/$LOGIN"
         * directory instead. */
        register char *user, *login, *subdir;

        user = getenv("USER");
        login = getenv("LOGIN");
        subdir = (user ? user : login);

        if (subdir) {
            for (i = 0; i < size && prefix[i]; i++)
                fullname[i] = prefix[i];

            fullname[i++] = '/';

            for (j = 0; i < size && subdir[j]; i++, j++)
                fullname[i] = subdir[j];
        } else {
            /* We didn't append the prefix yet */
            strncpy(fullname, prefix, MIN(strlen(prefix), size));
        }

        if ((strlen(fullname) + strlen(base_name) + 1) < size) {
            /* Append the base_name with a slash first. Multiple slashes are
             * handled below. */
            struct stat buf;

            if (stat(fullname, &buf) < 0)
                /* The directory doesn't exist just yet */
                if (mkdir(fullname, (mode_t)0755) < 0 && errno != EEXIST) {
                    /* We couldn't make the "/tmp/${USER,LOGIN}" subdirectory.
                     * Default to PREFIX's original prefix value. */
                    strcpy(fullname, prefix);
                }

            strcat(fullname, "/");
            strcat(fullname, base_name);
        } else {
            /* Buffer is too small */
            return NULL;
        }
    } else if (strlen(base_name) >= size) {
        /* Buffer is too small */
        return NULL;
    } else {
        strcpy(fullname, base_name);
    }

    /* Append a suffix */
    if (suffix) {
        if (strlen(fullname) + strlen(suffix) >= size)
            return NULL;

        strcat(fullname, suffix);
    }

    /* Remove any double slashes in the filename */
    for (ptr = fullname, i = j = 0; ptr && i < size; i++, ptr++) {
        if (*ptr != '/' || last != '/')
            fullname[j++] = *ptr;

        last = *ptr;
    }

    return fullname;
}

/*
 * Function:    do_write
 * Purpose:     Write the required amount of data to the file.
 * Return:      SUCCESS or FAIL
 * Programmer:  Bill Wendling, 14. November 2001
 * Modifications:
 */
static herr_t
do_write(file_descr *fd, iotype iot, long ndsets,
         long nelmts, long buf_size, char *buffer, MPI_Comm comm)
{
    int		ret_code = SUCCESS;
    int		rc;             /*routine return code                   */
    long	ndset;
    long	nelmts_towrite, nelmts_written;
    char	dname[64];
    off_t	dset_offset;          /*dataset offset in a file  */
    long	dset_size;    /*one dataset size in bytes */
    long	nelmts_in_buf;
    int         myrank, nprocs = 1;

    /* HDF5 variables */
    herr_t          hrc;                    /*HDF5 return code              */
    hsize_t         h5dims[1];              /*dataset dim sizes             */
    hid_t           h5dset_space_id = -1;   /*dataset space ID              */
    hid_t           h5mem_space_id = -1;    /*memory dataspace ID           */
    hid_t	    h5ds_id = -1;     /* dataset handle   */

    /* determine the mpi rank of in this comm */
    MPI_Comm_size(comm, &nprocs);
    MPI_Comm_rank(comm, &myrank);

    /* calculate dataset parameters. data type is always native C int */
    dset_size = nelmts * ELMT_SIZE;
    nelmts_in_buf = buf_size/ELMT_SIZE;

    /* hdf5 dataset setup */
    if (iot == PHDF5){
        /* define a contiquous dataset of nelmts native ints */
        h5dims[0] = nelmts;
        h5dset_space_id = H5Screate_simple(1, h5dims, NULL);
        VRFY((h5dset_space_id >= 0), "H5Screate_simple");

        /* create the memory dataspace */
        h5dims[0] = nelmts_in_buf;
        h5mem_space_id = H5Screate_simple(1, h5dims, NULL);
        VRFY((h5mem_space_id >= 0), "H5Screate_simple");
    }

    for (ndset = 1; ndset <= ndsets; ++ndset) {

        /* Calculate dataset offset within a file */

        /* create dataset */
        switch (iot) {
        case RAW:
        case MPIO:
            /* both raw and mpi io just need dataset offset in file*/
            dset_offset = (ndset - 1) * dset_size;
            break;

        case PHDF5:
            sprintf(dname, "Dataset_%lu", ndset);
            h5ds_id = H5Dcreate(fd->h5fd, dname, H5T_NATIVE_INT,
                                h5dset_space_id, H5P_DEFAULT);

            if (h5ds_id < 0) {
                fprintf(stderr, "HDF5 Dataset Create failed\n");
                GOTOERROR(FAIL);
            }

            break;
        }

        nelmts_written = 0 ;

        while (nelmts_written < nelmts){
            nelmts_towrite = nelmts - nelmts_written;

            if (nelmts - nelmts_written >= nelmts_in_buf) {
                nelmts_towrite = nelmts_in_buf;
            } else {
                /* last write of a partial buffer */
                nelmts_towrite = nelmts - nelmts_written;
            }

            /*Prepare write data*/
            {
                int *intptr = (int *)buffer;
                register int i;

                for (i = 0; i < nelmts_towrite; ++i)
                    *intptr++ = nelmts_towrite + i;
            }

            /* Write */

            /* Calculate offset of write within a dataset/file */
            switch (iot){
            case RAW:
                rc = RAWSEEK(fd->rawfd, dset_offset + nelmts_written*ELMT_SIZE);
		VRFY((rc>=0), "RAWSEEK");
                rc = RAWWRITE(fd->rawfd, buffer, nelmts_towrite*ELMT_SIZE);
		VRFY((rc==(nelmts_towrite*ELMT_SIZE)), "RAWWRITE");
                break;

            case MPIO:
            case PHDF5:
                break;
            }

            nelmts_written += nelmts_towrite;
        }

        /* Calculate write time */

        /* Close dataset. Only HDF5 needs to do an explicit close. */
        if (iot == PHDF5){
            hrc = H5Dclose(h5ds_id);

            if (hrc < 0) {
                fprintf(stderr, "HDF5 Dataset Close failed\n");
                GOTOERROR(FAIL);
            }

            h5ds_id = -1;
        }
    }

done:
    /* release HDF5 objects */
    if (h5dset_space_id != -1) {
        hrc = H5Sclose(h5dset_space_id);
        if (hrc < 0){
            fprintf(stderr, "HDF5 Dataset Space Close failed\n");
            ret_code = FAIL;
        } else {
            h5dset_space_id = -1;
        }
    }

    if (h5mem_space_id != -1) {
        hrc = H5Sclose(h5mem_space_id);
        if (hrc < 0) {
            fprintf(stderr, "HDF5 Memory Space Close failed\n");
            ret_code = FAIL;
        } else {
            h5mem_space_id = -1;
        }
    }

    return ret_code;
}

/*
 * Function:    do_fopen
 * Purpose:     Open the specified file.
 * Return:      SUCCESS or FAIL
 * Programmer:  Bill Wendling, 14. November 2001
 * Modifications:
 */
static herr_t
do_fopen(iotype iot, char *fname, file_descr *fd /*out*/, int flags,
    MPI_Comm comm)
{
    int ret_code = SUCCESS, mrc;
    herr_t hrc;
    hid_t acc_tpl = -1;     /* file access templates */

    switch (iot) {
    case RAW:
        if ((flags | PIO_CREATE) || (flags | PIO_WRITE)) {
            fd->rawfd = RAWCREATE(fname);
        } else {
            fd->rawfd = RAWOPEN(fname, O_RDONLY);
        }

        if (fd->rawfd < 0 ) {
            fprintf(stderr, "Raw File Open failed(%s)\n", fname);
            GOTOERROR(FAIL);
        }

        break;

    case MPIO:
        if ((flags | PIO_CREATE) || (flags | PIO_WRITE)) {
            mrc = MPI_File_open(comm, fname, MPI_MODE_CREATE | MPI_MODE_RDWR,
                                MPI_INFO_NULL, &(fd->mpifd));
        } else {
            mrc = MPI_File_open(comm, fname, MPI_MODE_RDONLY,
                                MPI_INFO_NULL, &(fd->mpifd));
        }

        if (mrc != MPI_SUCCESS) {
            fprintf(stderr, "MPI File Open failed(%s)\n", fname);
            GOTOERROR(FAIL);
        }

        break;

    case PHDF5:
        acc_tpl = H5Pcreate(H5P_FILE_ACCESS);

        if (acc_tpl < 0) {
            fprintf(stderr, "HDF5 Property List Create failed\n");
            GOTOERROR(FAIL);
        }

        hrc = H5Pset_fapl_mpio(acc_tpl, comm, MPI_INFO_NULL);     

        if (hrc < 0) {
            fprintf(stderr, "HDF5 Property List Set failed\n");
            GOTOERROR(FAIL);
        }

        /* create the parallel file */
        if ((flags | PIO_CREATE) || (flags | PIO_WRITE)) {
            fd->h5fd = H5Fcreate(fname, H5F_ACC_TRUNC, H5P_DEFAULT, acc_tpl);
        } else {
            fd->h5fd = H5Fopen(fname, H5P_DEFAULT, acc_tpl);
        }

        hrc = H5Pclose(acc_tpl);

        if (fd->h5fd < 0) {
            fprintf(stderr, "HDF5 File Create failed(%s)\n", fname);
            GOTOERROR(FAIL);
        }

        /* verifying the close of the acc_tpl */
        if (hrc < 0) {
            fprintf(stderr, "HDF5 Property List Close failed\n");
            GOTOERROR(FAIL);
        }

        break;
    } 

done:
    return ret_code;
}

/*
 * Function:    do_fclose
 * Purpose:     Close the specified file descriptor.
 * Return:      SUCCESS or FAIL
 * Programmer:  Bill Wendling, 14. November 2001
 * Modifications:
 */
static herr_t
do_fclose(iotype iot, file_descr *fd /*out*/)
{
    herr_t ret_code = SUCCESS, hrc;
    int mrc = 0, rc = 0;

    switch (iot) {
    case RAW:
        rc = RAWCLOSE(fd->rawfd);

        if (rc != 0){
            fprintf(stderr, "Raw File Close failed\n");
            GOTOERROR(FAIL);
        }

        fd->rawfd = -1;
        break;

    case MPIO:
        mrc = MPI_File_close(&(fd->mpifd));

        if (mrc != MPI_SUCCESS){
            fprintf(stderr, "MPI File close failed\n");
            GOTOERROR(FAIL);
        }

        fd->mpifd = MPI_FILE_NULL;
        break;

    case PHDF5:
        hrc = H5Fclose(fd->h5fd);

        if (hrc < 0) {
            fprintf(stderr, "HDF5 File Close failed\n");
            GOTOERROR(FAIL);
        }

        fd->h5fd = -1;
        break;
    }

done:
    return ret_code;
}

#endif /* H5_HAVE_PARALLEL */
