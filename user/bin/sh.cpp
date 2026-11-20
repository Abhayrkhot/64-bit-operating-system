// A small interactive shell on the serial console.
#include "libc.hpp"

namespace {

char g_cwd[256] = "/";

// Resolves `arg` against the working directory into out.
void resolve(const char* arg, char* out, size_t n) {
  if (arg[0] == '/') {
    snprintf(out, n, "%s", arg);
  } else if (!strcmp(g_cwd, "/")) {
    snprintf(out, n, "/%s", arg);
  } else {
    snprintf(out, n, "%s/%s", g_cwd, arg);
  }
}

int64_t read_command(char* buf, uint64_t size) {
  uint64_t n = 0;
  for (;;) {
    char c;
    if (read(0, &c, 1) <= 0) return -1;
    if (c == '\n' || c == '\r') {
      write(1, "\n", 1);
      break;
    }
    if (c == 0x7f || c == 0x08) {  // backspace
      if (n) {
        n--;
        write(1, "\b \b", 3);
      }
      continue;
    }
    if (n + 1 < size && c >= 0x20) {
      buf[n++] = c;
      write(1, &c, 1);
    }
  }
  buf[n] = 0;
  return static_cast<int64_t>(n);
}

int split(char* line, char** argv, int max) {
  int argc = 0;
  for (char* p = line; *p && argc < max;) {
    while (*p == ' ') *p++ = 0;
    if (!*p) break;
    argv[argc++] = p;
    while (*p && *p != ' ') p++;
  }
  argv[argc] = nullptr;
  return argc;
}

void cmd_ls(const char* dir) {
  int fd = open(dir, sys::kORead);
  if (fd < 0) {
    printf("ls: %s: %s\n", dir, errno_name(fd));
    return;
  }
  sys::DirEntry e;
  for (uint64_t i = 0; readdir(fd, &e, i) == 1; i++) {
    char p[320];
    snprintf(p, sizeof(p), "%s/%s", strcmp(dir, "/") ? dir : "", e.name);
    sys::Stat st{};
    stat(p, &st);
    printf("%c %10lu  %s\n", e.type == sys::kTypeDir ? 'd' : '-', st.size, e.name);
  }
  close(fd);
}

void cmd_cat(const char* path) {
  int fd = open(path, sys::kORead);
  if (fd < 0) {
    printf("cat: %s: %s\n", path, errno_name(fd));
    return;
  }
  char buf[1024];
  int64_t n;
  while ((n = read(fd, buf, sizeof(buf))) > 0) write(1, buf, static_cast<uint64_t>(n));
  close(fd);
}

void help() {
  puts("commands: ls [dir]  cat <file>  echo <text> [> file | >> file]  mkdir <dir>  rm <path>");
  puts("          mv <from> <to>  stat <path>  cd <dir>  pwd  sync  free  help  exit");
  puts("          any program in /bin (or a path): e.g. hello 1 2, usertest, fstest, sysfuzz 10000");
}

}  // namespace

int main(int, char**) {
  printf("\nShell ready. Type 'help'.\n");
  char line[256];
  char* argv[33];
  for (;;) {
    printf("%s$ ", g_cwd);
    if (read_command(line, sizeof(line)) < 0) return 0;
    // echo redirection: split off "> file" / ">> file" first.
    char* redirect = strchr(line, '>');
    bool append = false;
    char target[256] = {};
    if (redirect) {
      *redirect++ = 0;
      if (*redirect == '>') {
        append = true;
        redirect++;
      }
      while (*redirect == ' ') redirect++;
      resolve(redirect, target, sizeof(target));
    }
    int argc = split(line, argv, 32);
    if (argc == 0) continue;
    const char* cmd = argv[0];
    char path[256], path2[256];
    if (!strcmp(cmd, "exit")) return 0;
    if (!strcmp(cmd, "help")) {
      help();
    } else if (!strcmp(cmd, "pwd")) {
      puts(g_cwd);
    } else if (!strcmp(cmd, "cd")) {
      resolve(argc > 1 ? argv[1] : "/", path, sizeof(path));
      sys::Stat st;
      if (stat(path, &st) == 0 && st.type == sys::kTypeDir)
        snprintf(g_cwd, sizeof(g_cwd), "%s", path);
      else
        printf("cd: %s: not a directory\n", path);
    } else if (!strcmp(cmd, "ls")) {
      resolve(argc > 1 ? argv[1] : ".", path, sizeof(path));
      if (argc < 2) snprintf(path, sizeof(path), "%s", g_cwd);
      cmd_ls(path);
    } else if (!strcmp(cmd, "cat") && argc > 1) {
      resolve(argv[1], path, sizeof(path));
      cmd_cat(path);
    } else if (!strcmp(cmd, "echo")) {
      char text[256] = {};
      size_t n = 0;
      for (int i = 1; i < argc; i++) n += static_cast<size_t>(snprintf(text + n, sizeof(text) - n, "%s%s", i > 1 ? " " : "", argv[i]));
      if (n < sizeof(text) - 1) text[n++] = '\n';
      if (target[0]) {
        int fd = open(target, sys::kOWrite | sys::kOCreate | (append ? sys::kOAppend : sys::kOTruncate));
        if (fd < 0)
          printf("echo: %s: %s\n", target, errno_name(fd));
        else {
          write(fd, text, n);
          close(fd);
        }
      } else {
        write(1, text, n);
      }
    } else if (!strcmp(cmd, "mkdir") && argc > 1) {
      resolve(argv[1], path, sizeof(path));
      int r = mkdir(path);
      if (r < 0) printf("mkdir: %s\n", errno_name(r));
    } else if (!strcmp(cmd, "rm") && argc > 1) {
      resolve(argv[1], path, sizeof(path));
      int r = unlink(path);
      if (r < 0) printf("rm: %s\n", errno_name(r));
    } else if (!strcmp(cmd, "mv") && argc > 2) {
      resolve(argv[1], path, sizeof(path));
      resolve(argv[2], path2, sizeof(path2));
      int r = rename(path, path2);
      if (r < 0) printf("mv: %s\n", errno_name(r));
    } else if (!strcmp(cmd, "stat") && argc > 1) {
      resolve(argv[1], path, sizeof(path));
      sys::Stat st;
      int r = stat(path, &st);
      if (r < 0)
        printf("stat: %s\n", errno_name(r));
      else
        printf("%s: %s, %lu bytes, inode %u, %lu blocks\n", path, st.type == sys::kTypeDir ? "directory" : "file",
               st.size, st.inode, st.blocks);
    } else if (!strcmp(cmd, "sync")) {
      sync();
    } else if (!strcmp(cmd, "free")) {
      sys::SysInfo info;
      sysinfo(&info);
      printf("memory: %lu MiB free of %lu MiB; %lu processes; up %lu ms\n", info.free_bytes >> 20,
             info.total_bytes >> 20, info.processes, info.uptime_ns / 1000000);
    } else {
      if (strchr(cmd, '/'))
        resolve(cmd, path, sizeof(path));
      else
        snprintf(path, sizeof(path), "/bin/%s", cmd);
      int pid = spawn(path, argv);
      if (pid < 0) {
        printf("%s: %s\n", cmd, errno_name(pid));
        continue;
      }
      int status = 0;
      wait(pid, &status);
      if (status) printf("[exit %d]\n", status);
    }
  }
}
