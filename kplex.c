/* kplex: An anything to anything boat data multiplexer for Linux
 * Currently this program only supports nmea-0183 data.
 * For currently supported interfaces see kplex_mods.h
 * Copyright Keith Young 2012
 * For copying information, see the file COPYING distributed with this file
 */

/* This file (kplex.c) contains the main body of the program and
 * central multiplexing engine. Initialisation and read/write routines are
 * defined in interface-specific files
 */

#include "kplex.h"
#include "kplex_mods.h"
#include <signal.h>
#include <unistd.h>
#include <pwd.h>
#include <syslog.h>


/* Globals. Sadly. Used in signal handlers so few other simple options */
pthread_key_t ifkey;    /* Key for Thread local pointer to interface struct */
int timetodie=0;        /* Set on receipt of SIGTERM or SIGINT */

/* Signal handlers */
/* This one is used on receipt of SIGUSR1, used to tell an input interface
 * handler thread to terminate.  Output thread termination is handled a little
 * more gently giving them the opportunity to transmit buffered data */
void terminate (int sig)
{
    iface_thread_exit(sig);
}

/*  Signal handler for externally generated termination signals: Current
 * implementation this handles SIGTERM and SIGINT */
void killemall (int sig)
{
    struct iolists *listp;

    listp = (struct iolists *) pthread_getspecific(ifkey);
    timetodie++;
    pthread_cond_signal(&listp->dead_cond);
}

/* functions */
/*
 * Exit function used by interface handlers.  Interface objects are cleaned
 * up by the destructor funcitons of thread local storage
 * Args: exit status (unused)
 * Returns: Nothing
 */
void iface_thread_exit(int ret)
{
    pthread_exit((void *)&ret);
}

/*
 * Cleanup routine for interfaces, used as destructor for pointer to interface
 * structure in the handler thread's local storage
 * Args: pointer to interface structure
 * Returns: Nothing
 */
void iface_destroy(void *ifptr)
{
    iface_t *ifa = (iface_t *) ifptr;

    unlink_interface(ifa);
}

/*
 *  Initialise an ioqueue
 *  Args: size of queue (in senblk structures)
 *  Returns: pointer to new queue
 */
ioqueue_t *init_q(size_t size)
{
    ioqueue_t *newq;
    senblk_t *sptr;
    int    i;
    if ((newq=(ioqueue_t *)malloc(sizeof(ioqueue_t))) == NULL)
        return(NULL);
    if ((newq->base=(senblk_t *)calloc(size,sizeof(senblk_t))) ==NULL) {
        free(newq);
        return(NULL);
    }

    /* "base" always points to the allocated memory so that we can free() it.
     * All senblks initially allocated to the free list
     */
    newq->free=newq->base;

    /* Initiailise senblk queue pointers */
    for (i=0,sptr=newq->free,--size;i<size;++i,++sptr)
        sptr->next=sptr+1;

    sptr->next = NULL;

    newq->qhead = newq->qtail = NULL;

    pthread_mutex_init(&newq->q_mutex,NULL);
    pthread_cond_init(&newq->freshmeat,NULL);

    newq->active=1;
    return(newq);
}

/*
 *  Copy information in a senblk structure (data and len only)
 *  Args: pointers to dest and source senblk structures
 *  Returns: pointer to dest senblk
 */
senblk_t *senblk_copy(senblk_t *dptr,senblk_t *sptr)
{
    dptr->len=sptr->len;
    dptr->src=sptr->src;
    dptr->next=NULL;
    return (senblk_t *) memcpy((void *)dptr->data,(const void *)sptr->data,
            sptr->len);
}

/*
 * Add an senblk to an ioqueue
 * Args: Pointer to senblk and Pointer to queue it is to be added to
 * Returns: None
 */
void push_senblk(senblk_t *sptr, ioqueue_t *q)
{
    senblk_t *tptr;

    pthread_mutex_lock(&q->q_mutex);

    if (sptr == NULL) {
        /* NULL senblk pointer is magic "off" switch for a queue */
        q->active = 0;
    } else {
        /* Get a senblk from the queue's free list if possible...*/
        if (q->free) {
            tptr=q->free;
            q->free=q->free->next;
        } else {
            /* ...if not steal from the head of the queue, dropping previous
               contents. Should probably keep a counter for this */
            tptr=q->qhead;
            q->qhead=q->qhead->next;
        }
    
        (void) senblk_copy(tptr,sptr);
    
        /* If there is anything on the queue already, set it's "next" member
           to point to the new senblk */
        if (q->qtail)
            q->qtail->next=tptr;
    
        /* Set tail pointer to the new senblk */
        q->qtail=tptr;
    
        /* queue head needs to point to new senblk if there was nothing
           previously on the queue */
        if (q->qhead == NULL)
            q->qhead=tptr;
    
    }
    pthread_cond_broadcast(&q->freshmeat);
    pthread_mutex_unlock(&q->q_mutex);
}

/*
 *  Get the next senblk from the head of a queue
 *  Args: Queue to retrieve from
 *  Returns: Pointer to next senblk on the queue or NULL if the queue is
 *  no longer active
 *  This function blocks until data are available or the queue is shut down
 */
senblk_t *next_senblk(ioqueue_t *q)
{
    senblk_t *tptr;

    pthread_mutex_lock(&q->q_mutex);
    while ((tptr = q->qhead) == NULL) {
        /* No data available for reading */
        if (!q->active) {
            /* Return NULL if the queue has been sgut down */
            pthread_mutex_unlock(&q->q_mutex);
            return ((senblk_t *)NULL);
        }
        /* Wait until something is available */
        pthread_cond_wait(&q->freshmeat,&q->q_mutex);
    }

    /* set qhead to next element (which may be NULL)
       If the last element in the queue, set the tail pointer to NULL too */
    if ((q->qhead=tptr->next) == NULL)
        q->qtail=NULL;
    pthread_mutex_unlock(&q->q_mutex);
    return(tptr);
}

/*
 * Return a senblk to a queue's free list
 * Args: pointer to senblk, and pointer to the queue whose free list it is to
 * be added to
 * Returns: Nothing
 */
void senblk_free(senblk_t *sptr, ioqueue_t *q)
{
    pthread_mutex_lock(&q->q_mutex);
    /* Adding to head of free list is quicker than tail */
    sptr->next = q->free;
    q->free=sptr;
    pthread_mutex_unlock(&q->q_mutex);
}

iface_t *get_default_global()
{
    iface_t *ifp;

    if ((ifp = (iface_t *) malloc(sizeof(iface_t))) == NULL)
        return(NULL);

    ifp->type = GLOBAL;
    ifp->options = NULL;

    return(ifp);
}

/*
 * This is the heart of the multiplexer.  All inputs add to the tail of the
 * Engine's queue.  The engine takes from the head of its queue and copies
 * to all outputs on its output list.
 * Args: Pointer to information structure (iface_t, cast to void)
 * Returns: Nothing
 */
void *engine(void *info)
{
    senblk_t *sptr;
    iface_t *optr;
    iface_t *eptr = (iface_t *)info;
    int retval=0;

    (void) pthread_detach(pthread_self());

    for (;;) {
        sptr = next_senblk(eptr->q);
        pthread_mutex_lock(&eptr->lists->io_mutex);
        /* Traverse list of outputs and push a copy of senblk to each */
        for (optr=eptr->lists->outputs;optr;optr=optr->next) {
            if ((optr->direction == OUT) && ((!sptr) || (sptr->src != optr->pair))) {
                push_senblk(sptr,optr->q);
            }
        }
        pthread_mutex_unlock(&eptr->lists->io_mutex);
        if (sptr==NULL)
            /* Queue has been marked inactive */
            break;
        senblk_free(sptr,eptr->q);
    }
    pthread_exit(&retval);
}

/*
 * STart processing an interface and add it to an iolist, input or output, 
 * depending on direction
 * Args: Pointer to interface structure (cast to void *)
 * Returns: Nothing
 * We should come into this with SIGUSR1 blocked
 */
void start_interface(void *ptr)
{
    iface_t *ifa = (iface_t *)ptr;
    iface_t **lptr;
    iface_t **iptr;
    int ret=0;
    sigset_t set;

    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);

    pthread_mutex_lock(&ifa->lists->io_mutex);
    ifa->tid = pthread_self();

    if (pthread_setspecific(ifkey,ptr)) {
        perror("Falied to set key");
        exit(1);
    }

    if (ifa->direction == NONE) {
        pthread_mutex_unlock(&ifa->lists->io_mutex);
        iface_thread_exit(0);
    }

    for (iptr=&ifa->lists->initialized;*iptr!=ifa;iptr=&(*iptr)->next)
        if (*iptr == NULL) {
            perror("interface does not exist on initialized list!");
            exit(1);
        }

    *iptr=(*iptr)->next;

    /* Set lptr to point to the input or output list, as appropriate */
    lptr=(ifa->direction==IN)?&ifa->lists->inputs:&ifa->lists->outputs;
    if (*lptr)
        ifa->next=(*lptr);
    else
        ifa->next=NULL;
    (*lptr)=ifa;

    if (ifa->direction==BOTH) {
        if (ifa->lists->inputs)
            ifa->next=ifa->lists->inputs;
        else
            ifa->next=NULL;
        ifa->lists->inputs=ifa;
    }

    if (ifa->lists->initialized == NULL)
        pthread_cond_signal(&ifa->lists->init_cond);
    pthread_mutex_unlock(&ifa->lists->io_mutex);
    pthread_sigmask(SIG_UNBLOCK,&set,NULL);
    if (ifa->direction == IN)
        ifa->read(ifa);
    else
        ifa->write(ifa);
}

/*
 * link an interface into the initialized list
 * Args: interface structure pointer
 * Returns: 0 on success. There is no failure condition
 * Side Effects: links interface to the initialized list
 */
int link_to_initialized(iface_t *ifa)
{
    iface_t **iptr;

    pthread_mutex_lock(&ifa->lists->io_mutex);
    for (iptr=&ifa->lists->initialized;(*iptr);iptr=&(*iptr)->next);
    (*iptr)=ifa;
    ifa->next=NULL;
    pthread_mutex_unlock(&ifa->lists->io_mutex);
    return(0);
}

/*
 * Take an interface off the input or output iolist and place it on the "dead"
 * list waiting to be cleaned up
 * Args: Pointer to interface structure
 * Returns: 0 on success. Might add other possible return vals later
 * Should this be broken into link from input/output then link to dead?
 */
int unlink_interface(iface_t *ifa)
{
    iface_t **lptr;
    iface_t *tptr;

    sigset_t set,saved;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &set, &saved);
    pthread_mutex_lock(&ifa->lists->io_mutex);
    /* Set lptr to point to the input or output list, as appropriate */
    lptr=(ifa->direction==IN)?&ifa->lists->inputs:&ifa->lists->outputs;
    if ((*lptr) == ifa) {
        /* If target interface is the head of the list, set the list pointer
           to point to the next interface in the list */
        (*lptr)=(*lptr)->next;
    } else {
        /* Traverse the list until we find the interface before our target and
           make its next pointer point to the element after our target */
        for (tptr=(*lptr);tptr->next != ifa;tptr=tptr->next);
        tptr->next = ifa->next;
    }

    /* Unlink from both lists for BOTH interfaces */
    if (ifa->direction==BOTH) {
        if (ifa->lists->inputs == ifa)
            ifa->lists->inputs=ifa->next;
        else {
            for (tptr=ifa->lists->inputs;tptr->next != ifa;tptr=tptr->next);
            tptr->next = ifa->next;
        } 
    }
            
    if ((ifa->direction == OUT) && ifa->q) {
        /* output interfaces have queues which need freeing */
        free(ifa->q->base);
        free(ifa->q);
    } else {
        if (!ifa->lists->inputs) {
        pthread_mutex_lock(&ifa->q->q_mutex);
            ifa->q->active=0;
            pthread_cond_broadcast(&ifa->q->freshmeat);
        pthread_mutex_unlock(&ifa->q->q_mutex);
        }
    }
    ifa->cleanup(ifa);
    free(ifa->info);
    if (ifa->pair) {
        ifa->pair->pair=NULL;
        if (ifa->pair->direction == OUT) {
            pthread_mutex_lock(&ifa->pair->q->q_mutex);
            ifa->pair->q->active=0;
            pthread_cond_broadcast(&ifa->pair->q->freshmeat);
            pthread_mutex_unlock(&ifa->pair->q->q_mutex);
        } else {
            ifa->pair->direction = NONE;
            if (ifa->pair->tid)
                pthread_kill(ifa->pair->tid,SIGUSR1);
        }
    }

    /* Add to the dead list */
    if ((tptr=ifa->lists->dead) == NULL)
        ifa->lists->dead=ifa;
    else {
        for(;tptr->next;tptr=tptr->next);
    tptr->next=ifa;
    }
    ifa->next=NULL;
    /* Signal the reaper thread */
    pthread_cond_signal(&ifa->lists->dead_cond);
    pthread_mutex_unlock(&ifa->lists->io_mutex);
    pthread_sigmask(SIG_SETMASK,&saved,NULL);
    return(0);
}

/*
 * Duplicate an interface
 * Used when creating IN/OUT pair for bidirectional communication
 * Args: pointer to interface to be duplicated
 * Returns: Pointer to duplicate interface
 */
iface_t *ifdup (iface_t *ifa)
{
    iface_t *newif;

    if ((newif=(iface_t *) malloc(sizeof(iface_t))) == (iface_t *) NULL)
        return(NULL);
    if (iftypes[ifa->type].ifdup_func) {
        if ((newif->info=(*iftypes[ifa->type].ifdup_func)(ifa->info)) == NULL) {
            free(newif);
            return(NULL);
        }
    } else
        newif->info = NULL;

    ifa->pair=newif;
    newif->pair=ifa;
    newif->next=NULL;
    newif->type=ifa->type;
    newif->lists=ifa->lists;
    newif->read=ifa->read;
    newif->write=ifa->write;
    newif->cleanup=ifa->cleanup;
    newif->options=NULL;
    return(newif);
}

/*
 * Return the path to the kplex config file
 * Args: None
 * Returns: pointer to name of config file
 *
 * First choice is conf file in user's home directory, seocnd is global
 */
char *get_def_config()
{
    char *confptr;
    char *buf;
    struct passwd *pw;

    if (confptr=getenv("KPLEXCONF"))
        return (confptr);
    if ((confptr=getenv("HOME")) == NULL)
        if (pw=getpwuid(getuid()))
            confptr=pw->pw_dir;
    if (confptr) {
        if ((buf = malloc(strlen(confptr)+strlen(KPLEXHOMECONF)+2)) == NULL) {
            perror("failed to allocate memory");
            exit(1);
        }
        strcpy(buf,confptr);
        strcat(buf,"/");
        strcat(buf,KPLEXHOMECONF);
        if (!access(buf,F_OK))
            return(buf);
        free(buf);
    }
    if (!access(KPLEXGLOBALCONF,F_OK))
        return(KPLEXGLOBALCONF);
    return(NULL);
}

/*
 * Translate a string like "local7" to a log facility like LOG_LOCAL7
 * Args: string representation of log facility
 * Returns: Numeric representation of log facility, or -1 if string doesn't
 * map to anything appropriate
 */
int string2facility(char *fac)
{
    int facnum;

    if (!strcasecmp(fac,"kern"))
        return(LOG_KERN);
    if (!strcasecmp(fac,"user"))
        return(LOG_USER);
    if (!strcasecmp(fac,"mail"))
        return(LOG_MAIL);
    if (!strcasecmp(fac,"daemon"))
        return(LOG_DAEMON);
    if (!strcasecmp(fac,"auth"))
        return(LOG_AUTH);
    if (!strcasecmp(fac,"syslog"))
        return(LOG_SYSLOG);
    if (!strcasecmp(fac,"lpr"))
        return(LOG_LPR);
    if (!strcasecmp(fac,"news"))
        return(LOG_NEWS);
    if (!strcasecmp(fac,"cron"))
        return(LOG_CRON);
    if (!strcasecmp(fac,"authpriv"))
        return(LOG_AUTHPRIV);
    if (!strcasecmp(fac,"ftp"))
        return(LOG_FTP);
    /* if we don't map to "localX" where X is 0-7, return error */
    if (strncasecmp(fac,"local",5) || (*fac + 6))
        return(-1);
    if ((facnum = (((int) *fac+5) - 32) < 16) || facnum > 23)
        return(-1);
    return(facnum<<3);
}
main(int argc, char ** argv)
{
    pthread_t tid;
    pid_t pid;
    char *config=NULL;
    iface_t  *e_info;
    iface_t *ifptr,*ifptr2;
    iface_t **tiptr;
    int opt,err=0;
    struct kopts *option;
    int qsize=0;
    int background=0;
    int logto=LOG_DAEMON;
    void *ret;
    sigset_t set,saved;
    struct iolists lists = {
        .io_mutex = PTHREAD_MUTEX_INITIALIZER,
        .dead_mutex = PTHREAD_MUTEX_INITIALIZER,
        .init_cond = PTHREAD_COND_INITIALIZER,
        .dead_cond = PTHREAD_COND_INITIALIZER,
    .init_cond = PTHREAD_COND_INITIALIZER,
    .initialized = NULL,
    .outputs = NULL,
    .inputs = NULL,
    .dead = NULL
    };

    /* command line argument processing */
    while ((opt=getopt(argc,argv,"bl:q:f:")) != -1) {
        switch (opt) {
            case 'b':
                background++;
                break;
            case 'l':
                if ((logto = string2facility(option->val)) < 0) {
                    fprintf(stderr,"Unknown log facility \'%s\' specified in config file\n",option->val);
                    err++;
                }
                break;
            case 'q':
                if ((qsize=atoi(optarg)) < 2) {
                    fprintf(stderr,"%s: Minimum qsize is 2\n",
                            argv[0]);
                    err++;
                }
                break;
            case 'f':
                config=optarg;
                break;
            default:
                fprintf(stderr, "Usage: %s [-b] [-l <log facility>] [-q <size> ] [ -f <config file>] [<interface specification> ...]\n",argv[0]);
                err++;
        }
    }

    if (err)
        exit(1);

    /* If a config file is specified by a commad line argument, read it.  If
     * not, look for a default config file unless told not to using "-f-" on the
     * command line
     */
    if ((config && (strcmp(config,"-"))) ||
            (!config && (config = get_def_config()))) {
        if ((e_info=parse_file(config)) == NULL) {
            fprintf(stderr,"Error parsing config file: %s\n",errno?
                    strerror(errno):"Syntax Error");
            exit(1);
        }
    } else
        /* global options for engine configuration are also returned in config
         * file parsing. If we didn't do that, get default options here */
        e_info = get_default_global();

    /* queue size is taken from (in order of preference), command line arg, 
     * config file option in [global] section, default */
   if (e_info->options) {
        for (option=e_info->options;option;option=option->next)
            if (!strcmp(option->var,"qsize")) {
                if (!qsize)
                    if(!(qsize = atoi(option->val))) {
                        fprintf(stderr,"Invalid queue size: %s\n",option->val);
                        exit(1);
                    }
            } else if (!strcmp(option->var,"mode")) {
                if (!background) {
                    if (!strcmp(option->val,"background"))
                        background++;
                    else
                        fprintf(stderr,"Warning: unrecognized mode \'%s\' specified in config file\n",option->val);
                }
            } else if (!strcmp(option->var,"logto")) {
                if ((logto = string2facility(option->val)) < 0) {
                    fprintf(stderr,"Unknown log facility \'%s\' specified in config file\n",option->val);
                    exit(1);
                }
            } else {
                fprintf(stderr,"Warning: Unrecognized option \'%s\' in config file\n",option->var);
            }
    }

    if ((e_info->q = init_q(qsize?qsize:DEFQUEUESZ)) == NULL) {
        perror("failed to initiate queue");
        exit(1);
    }

    e_info->lists = &lists;
    lists.engine=e_info;

    if (e_info->options)
        free_options(e_info->options);

    for (tiptr=&e_info->next;optind < argc;optind++) {
        if (!(ifptr=parse_arg(argv[optind]))) {
            fprintf(stderr,"Failed to parse interface specifier %s\n",
                    argv[optind]);
            exit(1);
        }
        ifptr->next=(*tiptr);
        (*tiptr)=ifptr;
        tiptr=&ifptr->next;
    }

    /* We choose to go into the background here before interface initialzation
     * rather than later. Disadvantage: Errors don't get fed back on stderr.
     * Advantage: We can close all the file descriptors now rather than pulling
     * then from under erroneously specified stdin/stdout etc.
     */

    if (background) {
         if ((pid = fork()) < 0) {
            perror("fork failed");
            exit(1);
        } else if (pid)
            exit(0);

        /* Continue here as child */

        /* Really should close all file descriptors. Harder to do in OS
         * independent way.  Just close the ones we know about for this cut
         */
        fclose(stdin);
        fclose(stdout);
        fclose(stderr);
        setsid();
        chdir("/");
        umask(0);
    }

    /* log to stderr or syslog, as appropriate */
    initlog(background?logto:-1);

    /* our list of "real" interfaces starts after the first which is the
     * dummy "interface" specifying the multiplexing engine
     * walk the list, initialising the interfaces.  Sometimes "BOTH" interfaces
     * are initialised to one IN and one OUT which then need to be linked back
     * into the list
     */
    for (ifptr=e_info->next,tiptr=&lists.initialized;ifptr;ifptr=ifptr2) {
        ifptr2 = ifptr->next;
        if ((ifptr=(*iftypes[ifptr->type].init_func)(ifptr)) == NULL) {
            logerr(0,"Failed to initialize Interface");
            timetodie++;
            break;
        }

        for (;ifptr;ifptr = ifptr->next) {
        /* This loop should be done once for IN or OUT interfaces twice for
         * interfaces where the initialisation routine has expanded them to an
         * IN/OUT pair.
         */
            if (ifptr->direction != OUT)
                ifptr->q=e_info->q;

            ifptr->lists = &lists;
            (*tiptr)=ifptr;
            tiptr=&ifptr->next;
            if (ifptr->next==ifptr2)
                ifptr->next=NULL;
        }
    }

        
    /* Create the key for thread local storage: in this case for a pointer to
     * the interface each thread is handling
     */
    if (pthread_key_create(&ifkey,iface_destroy)) {
        logerr(errno,"Error creating key");
        timetodie++;
    }

    if (timetodie) {
        for (ifptr=lists.initialized;ifptr;ifptr=ifptr2) {
            ifptr2=ifptr->next;
            iface_destroy(ifptr);
        }
        exit(1);
    }

    pthread_setspecific(ifkey,(void *)&lists);

    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGINT);
    pthread_sigmask(SIG_BLOCK, &set, &saved);
    signal(SIGINT,killemall);
    signal(SIGTERM,killemall);
    signal(SIGUSR1,terminate);
    pthread_create(&tid,NULL,engine,(void *) e_info);

    pthread_mutex_lock(&lists.io_mutex);
    for (ifptr=lists.initialized;ifptr;ifptr=ifptr->next) {
        /* Create a thread to run each interface */
            pthread_create(&tid,NULL,(void *)start_interface,(void *) ifptr);
    }

    pthread_sigmask(SIG_SETMASK, &saved,NULL);
    while (lists.initialized)
        pthread_cond_wait(&lists.init_cond,&lists.io_mutex);

    /* While there are remaining outputs, wait until something is added to the 
     * dead list, reap everything on the dead list and check for outputs again
     * until all the outputs have been reaped
     * Note that when there are no more inputs, we set the
     * engine's queue inactive causing it to set all the outputs' queues
     * inactive and shutting them down. Thus the last input exiting also shuts
     * everything down */
    while (lists.outputs || lists.inputs || lists.dead) {
        while (lists.dead  == NULL && !timetodie)
            pthread_cond_wait(&lists.dead_cond,&lists.io_mutex);
        if (timetodie || (lists.outputs == NULL)) {
            timetodie=0;
            for (ifptr=lists.inputs;ifptr;ifptr=ifptr->next) {
                pthread_kill(ifptr->tid,SIGUSR1);
            }
        }
        for (ifptr=lists.dead;ifptr;ifptr=lists.dead) {
            lists.dead=ifptr->next;
                pthread_join(ifptr->tid,&ret);
                free(ifptr);
        }
    }
    exit(0);
}