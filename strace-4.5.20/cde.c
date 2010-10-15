#include "cde.h"

#ifdef BLAH
            if ((child_pcb->regs.eax >= 0) &&
                (open_mode == O_RDONLY || open_mode == O_RDWR)) {
              struct stat st;
              EXITIF(stat(filename, &st));
              // check whether it's a REGULAR-ASS file
              if (S_ISREG(st.st_mode)) {
                // assume that relative paths are in working directory,
                // so no need to grab those files
                //
                // TODO: this isn't a perfect assumption since a
                // relative path could be something like '../data.txt',
                // which this won't pick up :)
                //   WOW, this libc function seems useful for
                //   canonicalizing filenames:
                //     char* canonicalize_file_name (const char *name)
                if (filename[0] == '/') {
                  // modify filename so that it appears as a RELATIVE PATH
                  // within a cde-root/ sub-directory
                  char* rel_path = malloc(strlen(filename) + strlen("cde-root") + 1);
                  strcpy(rel_path, "cde-root");
                  strcat(rel_path, filename);

                  struct path* p = str2path(rel_path);
                  path_pop(p); // ignore filename portion to leave just the dirname

                  // now mkdir all directories specified in rel_path
                  int i;
                  for (i = 1; i <= p->depth; i++) {
                    char* dn = path2str(p, i);
                    mkdir(dn, 0777);
                    free(dn);
                  }

                  // finally, 'copy' filename over to rel_path
                  // 1.) try a hard link for efficiency
                  // 2.) if that fails, then do a straight-up copy 
                  //     TODO: can optimize by first checking md5sum or
                  //     something before copying

                  // EEXIST means the file already exists, which isn't
                  // really a hard link failure ...
                  if (link(filename, rel_path) && (errno != EEXIST)) {
                    copy_file(filename, rel_path);
                  }

                  delete_path(p);
                  free(rel_path);
                }
              }
#endif


// used as a temporary holding space for paths copied from child process
static char path[MAXPATHLEN + 1]; 

void CDE_begin_file_open(struct tcb* tcp) {
  // only track files opened in read-only or read-write mode:
  char open_mode = (tcp->u_arg[1] & 0x3);
  if (open_mode == O_RDONLY || open_mode == O_RDWR) {
    EXITIF(umovestr(tcp, (long)tcp->u_arg[0], sizeof path, path) < 0);
    assert(!tcp->opened_filename);
    tcp->opened_filename = strdup(path);
  }
}

void CDE_end_file_open(struct tcb* tcp) {
  if (!tcp->opened_filename) {
    return;
  }
     
  // a non-negative return value means that the call returned
  // successfully with a known file descriptor
  if (tcp->u_rval >= 0) {
    printf("END   open %s\n", tcp->opened_filename);
  }

  free(tcp->opened_filename);
  tcp->opened_filename = NULL;
}

void CDE_begin_execve(struct tcb* tcp) {
  EXITIF(umovestr(tcp, (long)tcp->u_arg[0], sizeof path, path) < 0);
  assert(!tcp->opened_filename);
  tcp->opened_filename = strdup(path);
}

void CDE_end_execve(struct tcb* tcp) {
  assert(tcp->opened_filename);
 
  // a return value of 0 means success
  if (tcp->u_rval == 0) {
    printf("END   execve %s\n", tcp->opened_filename);
  }

  free(tcp->opened_filename);
  tcp->opened_filename = NULL;
}


// TODO: this is probably Linux-specific ;)
void* find_free_addr(int pid, int prot, unsigned long size) {
  FILE *f;
  char filename[20];
  char s[80];
  char r, w, x, p;

  sprintf(filename, "/proc/%d/maps", pid);

  f = fopen(filename, "r");
  if (!f) {
    fprintf(stderr, "Can not find a free address in pid %d: %s\n.", pid, strerror(errno));
  }
  while (fgets(s, sizeof(s), f) != NULL) {
    unsigned long cstart, cend;
    int major, minor;

    sscanf(s, "%lx-%lx %c%c%c%c %*x %d:%d", &cstart, &cend, &r, &w, &x, &p, &major, &minor);

    if (cend - cstart < size) {
      continue;
    }

    if (!(prot & FILEBACK) && (major || minor)) {
      continue;
    }

    if (p != 'p') {
      continue;
    }
    if ((prot & PROT_READ) && (r != 'r')) {
      continue;
    }
    if ((prot & PROT_EXEC) && (x != 'x')) {
      continue;
    }
    if ((prot & PROT_WRITE) && (w != 'w')) {
      continue;
    }
    fclose(f);

    return (void *)cstart;
  }
  fclose(f);

  return NULL;
}


struct pcb* new_pcb(int pid, int state) {
  key_t key;
  struct pcb* ret;

  ret = (struct pcb*)malloc(sizeof(*ret));
  if (!ret) {
    return NULL;
  }

  memset(ret, 0, sizeof(*ret));

  ret->pid = pid;
  ret->state = state;
  ret->prime_fd = -1;

  // randomly probe for a valid shm key
  do {
    errno = 0;
    key = rand();
    ret->shmid = shmget(key, PATH_MAX * 2, IPC_CREAT|IPC_EXCL|0600);
  } while (ret->shmid == -1 && errno == EEXIST);

  ret->localshm = (char*)shmat(ret->shmid, NULL, 0);
  if ((int)ret->localshm == -1) {
    perror("shmat");
    exit(1);
  }

  if (shmctl(ret->shmid, IPC_RMID, NULL) == -1) {
    perror("shmctl(IPC_RMID)");
    exit(1);
  }

  ret->childshm = NULL;

  return ret;
}

void delete_pcb(struct pcb *pcb) {
  shmdt(pcb->localshm);
  free(pcb);
}


// manipulating paths (courtesy of Goanna)

static void empty_path(struct path *path) {
  int pos = 0;
  path->depth = 0;
  if (path->stack) {
    while (path->stack[pos]) {
      free(path->stack[pos]);
      path->stack[pos] = NULL;
      pos++;
    }
  }
}

// mallocs a new path object, must free using delete_path(),
// NOT using ordinary free()
struct path* path_dup(struct path* path) {
  struct path* ret = (struct path *)malloc(sizeof(struct path));
  assert(ret);

  ret->stacksize = path->stacksize;
  ret->depth = path->depth;
  ret->is_abspath = path->is_abspath;
  ret->stack = (struct namecomp**)malloc(sizeof(struct namecomp*) * ret->stacksize);
  assert(ret->stack);

  int pos = 0;
  while (path->stack[pos]) {
    ret->stack[pos] =
      (struct namecomp*)malloc(sizeof(struct namecomp) +
                               strlen(path->stack[pos]->str) + 1);
    assert(ret->stack[pos]);
    ret->stack[pos]->len = path->stack[pos]->len;
    strcpy(ret->stack[pos]->str, path->stack[pos]->str);
    pos++;
  }
  ret->stack[pos] = NULL;
  return ret;
}

struct path *new_path() {
  struct path* ret = (struct path *)malloc(sizeof(struct path));
  assert(ret);

  ret->stacksize = 1;
  ret->depth = 0;
  ret->is_abspath = 0;
  ret->stack = (struct namecomp **)malloc(sizeof(struct namecomp *));
  assert(ret->stack);
  ret->stack[0] = NULL;
  return ret;
}

void delete_path(struct path *path) {
  assert(path);
  if (path->stack) {
    int pos = 0;
    while (path->stack[pos]) {
      free(path->stack[pos]);
      path->stack[pos] = NULL;
      pos++;
    }
    free(path->stack);
  }
  free(path);
}


// mallocs a new string and populates it with up to
// 'depth' path components (if depth is 0, uses entire path)
char* path2str(struct path* path, int depth) {
  int i;
  int destlen = 1;

  // simply use path->depth if depth is out of range
  if (depth <= 0 || depth > path->depth) {
    depth = path->depth;
  }

  for (i = 0; i < depth; i++) {
    destlen += path->stack[i]->len + 1;
  }

  char* dest = (char *)malloc(destlen);

  char* ret = dest;
  assert(destlen >= 2);

  if (path->is_abspath) {
    *dest++ = '/';
    destlen--;
  }

  for (i = 0; i < depth; i++) {
    assert(destlen >= path->stack[i]->len + 1);

    memcpy(dest, path->stack[i]->str, path->stack[i]->len);
    dest += path->stack[i]->len;
    destlen -= path->stack[i]->len;

    if (i < depth - 1) { // do we have a successor?
      assert(destlen >= 2);
      *dest++ = '/';
      destlen--;
    }
  }

  *dest = '\0';

  return ret;
}

// pop the last element of path
void path_pop(struct path* p) {
  if (p->depth == 0) {
    return;
  }

  free(p->stack[p->depth-1]);
  p->stack[p->depth-1] = NULL;
  p->depth--;
}

// mallocs a new path object, must free using delete_path(),
// NOT using ordinary free()
struct path* str2path(char* path) {
  int stackleft;

  path = strdup(path); // so that we don't clobber the original
  char* path_dup_base = path; // for free()

  struct path* base = new_path();

  if (*path == '/') { // absolute path?
    base->is_abspath = 1;
    empty_path(base);
    path++;
  }
  else {
    base->is_abspath = 0;
  }

  stackleft = base->stacksize - base->depth - 1;

  do {
    char *p;
    while (stackleft <= 1) {
      base->stacksize *= 2;
      stackleft = base->stacksize / 2;
      base->stacksize++;
      stackleft++;
      base->stack =
        (struct namecomp **)realloc(base->stack, base->stacksize * sizeof(struct namecomp*));
      assert(base->stack);
    }

    // Skip multiple adjoining slashes
    while (*path == '/') {
      path++;
    }

    p = strchr(path, '/');
    // put a temporary stop-gap ... uhhh, this assumes path isn't read-only
    if (p) {
      *p = '\0';
    }

    if (path[0] == '\0') {
      base->stack[base->depth] = NULL;
      // We are at the end (or root), do nothing.
    }
    else if (!strcmp(path, ".")) {
      base->stack[base->depth] = NULL;
      // This doesn't change anything.
    }
    else if (!strcmp(path, "..")) {
      if (base->depth > 0) {
        free(base->stack[--base->depth]);
        base->stack[base->depth] = NULL;
        stackleft++;
      }
    }
    else {
      base->stack[base->depth] =
        (struct namecomp *)malloc(sizeof(struct namecomp) + strlen(path) + 1);
      assert(base->stack[base->depth]);
      strcpy(base->stack[base->depth]->str, path);
      base->stack[base->depth]->len = strlen(path);
      base->depth++;
      base->stack[base->depth] = NULL;
      stackleft--;
    }

    // Put it back the way it was
    if (p) {
      *p++ = '/';
    }
    path = p;
  } while (path);

  free(path_dup_base);
  return base;
}


// primitive file copy
// TODO: could optimize by never clobbering dst_filename if its
// modification date is equal to or newer than that of src_filename
void copy_file(char* src_filename, char* dst_filename) {
  int inF;
  int outF;
  int bytes;
  char buf[4096]; // TODO: consider using BUFSIZ if it works better

  EXITIF((inF = open(src_filename, O_RDONLY)) < 0);
  // create with permissive perms
  EXITIF((outF = open(dst_filename, O_WRONLY | O_CREAT, 0777)) < 0);

  while ((bytes = read(inF, buf, sizeof(buf))) > 0) {
    write(outF, buf, bytes);
  }
    
  close(inF);
  close(outF);
}

