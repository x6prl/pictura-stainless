/*
 * Copyright (C) 2026 Viktor Filinkov
 *
 * This file is part of pictura-stainless.
 *
 * pictura-stainless is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * pictura-stainless is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with pictura-stainless.  If not, see <https://www.gnu.org/licenses/>.
 */
// SPDX-License-Identifier: GPL-3.0

#include <linux/limits.h>

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>

#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern char **environ;

constexpr const char PROGRAM_NAME[] = "pictura-stainless";
constexpr const char VERSION[] = "1.0.0";

typedef struct {
	dev_t dev;
	ino_t ino;
} Inode;

constexpr size_t MMAP_SIZE = (size_t)256 << 20; // 256 MiB

constexpr size_t PATH_SIZE_MAX = PATH_MAX;
constexpr size_t RECURSION_DEPTH_MAX = 1 << 10;
char *path_name_buffer[RECURSION_DEPTH_MAX] = {};

char *chosen_name_buffer = nullptr;

// NOTE: 8,388,608 slots * 16 bytes = 128 MiB
constexpr size_t HASH_CAPACITY = 1UL << 23;
constexpr size_t HASH_MASK = HASH_CAPACITY - 1;
constexpr size_t HASH_MAX_LOAD = (HASH_CAPACITY * 9) / 10; // 90% cap

Inode *visited_dirs = nullptr;
size_t visited_dirs_count = 0;

size_t imgs_count = 0;
size_t files_count = 0;
size_t dirs_count = 0;
size_t recursion_counter = 0;

uint64_t rng_state = 0;

constexpr unsigned long DEFAULT_QS_TIMEOUT_MS = 5000;
constexpr unsigned long MAX_QS_TIMEOUT_MS = 3600000; // 1 hour
constexpr const char ENV_TIMEOUT_MS[] = "PICTURA_STAINLESS_TIMEOUT_MS";

volatile sig_atomic_t timed_out = 0;

void alarm_handler([[maybe_unused]] int sig) { timed_out = 1; }

void print_usage(const char *prog_name) {
	fprintf(stderr,
	        "Usage: %s [OPTIONS] <directory>\n\n"
	        "Options:\n"
	        "  -r, --recursive  Recursively search subdirectories\n"
	        "  --dry-run        Do not set wallpaper\n"
	        "  -q, --quiet      Suppress informational output\n"
	        "  -p               Print only the chosen path (no newline) and do "
	        "not set wallpaper\n"
	        "  -v, --version    Display version information\n"
	        "  -h, --help       Display this help message\n\n"
	        "Environment Variables:\n"
	        "  %s  Timeout in milliseconds for qs IPC call (default: %lu)\n",
	        prog_name, ENV_TIMEOUT_MS, DEFAULT_QS_TIMEOUT_MS);
}

unsigned long load_timeout_ms() {
	const char *env = getenv(ENV_TIMEOUT_MS);
	if (!env || env[0] == '\0') {
		return DEFAULT_QS_TIMEOUT_MS;
	}

	char *endptr = nullptr;
	errno = 0;
	unsigned long val = strtoul(env, &endptr, 10);
	if (errno != 0 || endptr == env || *endptr != '\0' || val == 0) {
		fprintf(stderr,
		        "warning: invalid %s value '%s', using default %lu ms\n",
		        ENV_TIMEOUT_MS, env, DEFAULT_QS_TIMEOUT_MS);
		return DEFAULT_QS_TIMEOUT_MS;
	}

	if (val > MAX_QS_TIMEOUT_MS) {
		fprintf(stderr, "warning: %s value %lu exceeds max (%lu ms), capping\n",
		        ENV_TIMEOUT_MS, val, MAX_QS_TIMEOUT_MS);
		return MAX_QS_TIMEOUT_MS;
	}

	return val;
}

// NOTE: Xorshift64* PRNG
//      (Sebastiano Vigna, 2016, ACM TOMS, DOI: 10.1145/2845077)
uint64_t random_number(uint64_t to) {
	rng_state ^= rng_state >> 12;
	rng_state ^= rng_state << 25;
	rng_state ^= rng_state >> 27;
	return (rng_state * UINT64_C(2685821657736338717)) % to;
}

// NOTE: 64-bit hash combiner with Stafford's Mix13
//      (David Stafford, 2011; Steele et al., OOPSLA 2014)
uint64_t hash_inode(dev_t dev, ino_t ino) {
	uint64_t h = (uint64_t)dev ^ ((uint64_t)ino * UINT64_C(0x517cc1b727220a95));
	h ^= h >> 30;
	h *= UINT64_C(0xbf58476d1ce4e5b9);
	h ^= h >> 27;
	h *= UINT64_C(0x94d049bb133111eb);
	h ^= h >> 31;
	return h;
}

bool is_visited_dir(dev_t dev, ino_t ino) {
	for (uint64_t idx = hash_inode(dev, ino) & HASH_MASK;
	     visited_dirs[idx].ino != 0 || visited_dirs[idx].dev != 0;
	     idx = (idx + 1) & HASH_MASK) {
		if (visited_dirs[idx].dev == dev && visited_dirs[idx].ino == ino) {
			return true;
		}
	}
	return false;
}

bool set_visited_dir(dev_t dev, ino_t ino) {
	uint64_t idx = hash_inode(dev, ino) & HASH_MASK;
	for (; visited_dirs[idx].ino != 0 || visited_dirs[idx].dev != 0;
	     idx = (idx + 1) & HASH_MASK) {
		if (visited_dirs[idx].dev == dev && visited_dirs[idx].ino == ino) {
			return true;
		}
	}

	// NOTE: 90% capacity guard
	if (visited_dirs_count >= HASH_MAX_LOAD) {
		fprintf(stderr,
		        "warning: visited directory cache 90%% full (%zu entries), "
		        "skipping deep traversal: dev %lu ino %lu\n",
		        visited_dirs_count, (unsigned long)dev, (unsigned long)ino);
		return false;
	}

	visited_dirs[idx].dev = dev;
	visited_dirs[idx].ino = ino;
	++visited_dirs_count;
	return true;
}

const char *path_append(char *buf, const char *path, size_t path_len,
                        const char *name, size_t name_len, size_t *result_len) {
	memcpy(buf, path, path_len);
	buf[path_len] = '/';
	memcpy(buf + path_len + 1, name, name_len);
	*result_len = path_len + name_len + 1;
	buf[*result_len] = '\0';
	return buf;
}

// NOTE: big thanks to what-a-default-LLM-in-ChatGPT for the implementation
bool is_image32(const char *name) {
	const char *dot = strrchr(name, '.');
	if (!dot || !dot[1])
		return false;

	const char *ext = dot + 1;
	const size_t len = strlen(ext);

	if (len == 3) {
		const uint32_t v = ((uint32_t)(unsigned char)ext[0] | 0x20u) << 16 |
		                   ((uint32_t)(unsigned char)ext[1] | 0x20u) << 8 |
		                   ((uint32_t)(unsigned char)ext[2] | 0x20u);

		switch (v) {
		case ('j' << 16) | ('p' << 8) | 'g':
		case ('p' << 16) | ('n' << 8) | 'g':
		case ('g' << 16) | ('i' << 8) | 'f':
		case ('b' << 16) | ('m' << 8) | 'p':
		case ('t' << 16) | ('i' << 8) | 'f':
		case ('j' << 16) | ('x' << 8) | 'l':
			return true;
		default:
			return false;
		}
	}

	if (len == 4) {
		const uint32_t v = ((uint32_t)(unsigned char)ext[0] | 0x20u) << 24 |
		                   ((uint32_t)(unsigned char)ext[1] | 0x20u) << 16 |
		                   ((uint32_t)(unsigned char)ext[2] | 0x20u) << 8 |
		                   ((uint32_t)(unsigned char)ext[3] | 0x20u);

		switch (v) {
		case ('j' << 24) | ('p' << 16) | ('e' << 8) | 'g':
		case ('w' << 24) | ('e' << 16) | ('b' << 8) | 'p':
		case ('t' << 24) | ('i' << 16) | ('f' << 8) | 'f':
		case ('a' << 24) | ('v' << 16) | ('i' << 8) | 'f':
		case ('h' << 24) | ('e' << 16) | ('i' << 8) | 'c':
			return true;
		default:
			return false;
		}
	}

	return false;
}

bool is_special_path(const char *name) {
	const bool is_cur_dir = name[0] == '.' && name[1] == '\0';
	const bool is_par_dir = name[0] == '.' && name[1] == '.' && name[2] == '\0';
	return is_cur_dir || is_par_dir;
}

void add_if_image(const char *path, size_t path_len, const char *name) {
	++files_count;
	if (is_image32(name)) {
		size_t name_len = strlen(name);
		if (path_len + 1 + name_len >= PATH_SIZE_MAX) {
			return;
		}

		++imgs_count;
		// NOTE: Reservoir Sampling Algorithm R
		//      (Alan G. Waterman / Donald Knuth,
		//       TAOCP Vol. 2, Sec. 3.4.2)
		if (0 == random_number(imgs_count)) {
			size_t res_length = 0;
			path_append(chosen_name_buffer, path, path_len, name, name_len,
			            &res_length);
		}
	}
}

int walk(const char *path, size_t path_len, bool is_recursive) {
	if (recursion_counter >= RECURSION_DEPTH_MAX) {
		fprintf(stderr, "Max depth %zu achieved, skipping: %s\n",
		        (size_t)RECURSION_DEPTH_MAX, path);
		return 0;
	}
	auto dir = opendir(path);
	if (!dir) {
		fprintf(stderr, "I/O error: cannot open directory %s: %s\n", path,
		        strerror(errno));
		return -1;
	}

	struct stat st;
	if (fstat(dirfd(dir), &st) != 0) {
		fprintf(stderr, "I/O error: cannot stat %s: %s\n", path,
		        strerror(errno));
		closedir(dir);
		return -1;
	}

	if (is_visited_dir(st.st_dev, st.st_ino)) {
		closedir(dir);
		return 0;
	}

	// NOTE: visited list size reached cap, skip to prevent untracked loops
	if (!set_visited_dir(st.st_dev, st.st_ino)) {
		closedir(dir);
		return 0;
	}

	++dirs_count;

	for (auto entry = readdir(dir); entry; entry = readdir(dir)) {
		switch (entry->d_type) {
		case DT_REG:
			add_if_image(path, path_len, entry->d_name);
			break;
		case DT_LNK:
		case DT_UNKNOWN: {
			struct stat link_st;
			if (fstatat(dirfd(dir), entry->d_name, &link_st, 0) != 0) {
				break;
			}
			// a file:
			if (S_ISREG(link_st.st_mode)) {
				add_if_image(path, path_len, entry->d_name);
				break;
			}
			// a dir:
			if (S_ISDIR(link_st.st_mode)) {
				// NOTE: causes double check on symlinked dirs
				// drop?
				if (is_visited_dir(link_st.st_dev, link_st.st_ino)) {
					break;
				}
				[[fallthrough]]; // so we handle like a dir
			} else {
				// not a file nor dir
				break;
			}
		}
		case DT_DIR:
			if (is_recursive && !is_special_path(entry->d_name)) {
				size_t name_len = strlen(entry->d_name);
				if (path_len + 1 + name_len >= PATH_SIZE_MAX) {
					fprintf(stderr, "Path too long: %s/%s\n", path,
					        entry->d_name);
					break;
				}

				auto path_buf = path_name_buffer[recursion_counter];
				size_t res_length = 0;
				auto child_path =
					  path_append(path_buf, path, path_len, entry->d_name,
				                  name_len, &res_length);

				++recursion_counter;
				walk(child_path, res_length, is_recursive);
				--recursion_counter;
			}
			break;
		default:
			break;
		}
	}

	closedir(dir);
	return 0;
}

int set_wallpaper(const char *image_path, unsigned long timeout_ms) {
	char *const qs_argv[] = {
		  (char *)"qs",  (char *)"-c",       (char *)"noctalia-shell",
		  (char *)"ipc", (char *)"call",     (char *)"wallpaper",
		  (char *)"set", (char *)image_path, nullptr,
	};

	pid_t pid;
	int err = posix_spawnp(&pid, "qs", nullptr, nullptr, qs_argv, environ);
	if (err != 0) {
		if (err == ENOENT) {
			fprintf(stderr, "command 'qs' not found; make sure Quickshell is "
			                "installed\n");
			return 2;
		}
		fprintf(stderr, "failed to spawn 'qs': %s\n", strerror(err));
		return 2;
	}

	struct sigaction sa = {};
	sa.sa_handler = alarm_handler;
	sigaction(SIGALRM, &sa, nullptr);
	timed_out = 0;

	struct itimerval timer = {
		  .it_interval = {0, 0},
		  .it_value =
				{
					  .tv_sec = (time_t)(timeout_ms / 1000),
					  .tv_usec = (suseconds_t)((timeout_ms % 1000) * 1000),
				},
	};
	setitimer(ITIMER_REAL, &timer, nullptr);

	int status = 0;
	for (; waitpid(pid, &status, 0) == -1;) {
		if (errno == EINTR) {
			if (timed_out) {
				kill(pid, SIGKILL);
				waitpid(pid, nullptr, 0);
				fprintf(stderr, "'qs' did not finish within %lu ms\n",
				        timeout_ms);
				struct itimerval zero = {};
				setitimer(ITIMER_REAL, &zero, nullptr);
				signal(SIGALRM, SIG_DFL);
				return 1;
			}
			continue;
		}
		fprintf(stderr, "I/O error: waitpid failed: %s\n", strerror(errno));
		struct itimerval zero = {};
		setitimer(ITIMER_REAL, &zero, nullptr);
		signal(SIGALRM, SIG_DFL);
		return 1;
	}

	struct itimerval zero = {};
	setitimer(ITIMER_REAL, &zero, nullptr);
	signal(SIGALRM, SIG_DFL);

	if (WIFEXITED(status)) {
		int code = WEXITSTATUS(status);
		if (code == 127) {
			fprintf(stderr, "command 'qs' not found; make sure Quickshell is "
			                "installed\n");
			return 2;
		}
		if (code != 0) {
			fprintf(stderr, "Noctalia Shell failed: exit code %d\n", code);
			return 1;
		}
	} else if (WIFSIGNALED(status)) {
		fprintf(stderr, "Noctalia Shell failed: killed by signal %d\n",
		        WTERMSIG(status));
		return 1;
	}
	return 0;
}

int main(int argc, char *argv[]) {
	bool is_flag_dry_run = false;
	bool is_flag_quiet = false;
	bool is_flag_print_only = false;
	bool is_recursive = false;
	char *path = nullptr;

	for (int i = 1; i < argc; ++i) {
		char *arg = argv[i];
		if (strcmp(arg, "--dry-run") == 0) {
			is_flag_dry_run = true;
		} else if (strcmp(arg, "-q") == 0 || strcmp(arg, "--quiet") == 0) {
			is_flag_quiet = true;
		} else if (strcmp(arg, "-p") == 0) {
			is_flag_print_only = true;
		} else if (strcmp(arg, "-r") == 0 || strcmp(arg, "--recursive") == 0) {
			is_recursive = true;
		} else if (strcmp(arg, "-v") == 0 || strcmp(arg, "-V") == 0 ||
		           strcmp(arg, "--version") == 0) {
			printf("%s %s\n", PROGRAM_NAME, VERSION);
			return 0;
		} else if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
			print_usage(argv[0]);
			return 0;
		} else if (arg[0] == '-' && arg[1] != '\0' && arg[1] != '-') {
			for (size_t j = 1; arg[j] != '\0'; ++j) {
				if (arg[j] == 'q') {
					is_flag_quiet = true;
				} else if (arg[j] == 'p') {
					is_flag_print_only = true;
				} else if (arg[j] == 'r') {
					is_recursive = true;
				} else if (arg[j] == 'v' || arg[j] == 'V') {
					printf("%s %s\n", PROGRAM_NAME, VERSION);
					return 0;
				} else {
					fprintf(stderr, "unknown option: -%c\n", arg[j]);
					print_usage(argv[0]);
					return 1;
				}
			}
		} else if (arg[0] == '-') {
			fprintf(stderr, "unknown option: %s\n", arg);
			print_usage(argv[0]);
			return 1;
		} else {
			if (!path) {
				path = arg;
			} else {
				fprintf(stderr, "unexpected argument: %s\n", arg);
				print_usage(argv[0]);
				return 1;
			}
		}
	}

	if (!path) {
		print_usage(argv[0]);
		return 1;
	}

	struct stat root_st;
	if (stat(path, &root_st) != 0) {
		if (errno == ENOENT) {
			fprintf(stderr, "path does not exist: %s\n", path);
			return 1;
		}
		fprintf(stderr, "I/O error: %s: %s\n", path, strerror(errno));
		return 1;
	}

	if (!S_ISDIR(root_st.st_mode)) {
		fprintf(stderr, "not a directory: %s\n", path);
		return 1;
	}

	auto timeout_ms = load_timeout_ms();

	arc4random_buf(&rng_state, sizeof(rng_state));
	if (rng_state == 0) {
		rng_state = 1;
	}

	char *mem = (char *)mmap(nullptr, MMAP_SIZE, PROT_READ | PROT_WRITE,
	                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mem == MAP_FAILED) {
		fprintf(stderr, "I/O error: mmap failed: %s\n", strerror(errno));
		return 1;
	}

	for (size_t i = 0; i < RECURSION_DEPTH_MAX; ++i) {
		path_name_buffer[i] = mem;
		mem += PATH_SIZE_MAX;
	}

	chosen_name_buffer = mem;
	mem += PATH_SIZE_MAX;
	chosen_name_buffer[0] = '\0';

	visited_dirs = (Inode *)mem;

	size_t path_len = strlen(path);
	for (; path_len > 1 && path[path_len - 1] == '/'; --path_len) {
		path[path_len - 1] = '\0';
	}

	walk(path, path_len, is_recursive);

	if (imgs_count == 0) {
		fprintf(stderr, "no supported images found in %s\n", path);
		return 1;
	}

	if (is_flag_print_only) {
		printf("%s", chosen_name_buffer);
		fflush(stdout);
		return 0;
	}

	if (!is_flag_quiet) {
		printf("found %zu files in %zu checked directories\n", files_count, dirs_count);
		printf("total images found: %zu\n", imgs_count);
		printf("chosen image path: %s\n", chosen_name_buffer);
	}

	if (is_flag_dry_run) {
		return 0;
	}

	return set_wallpaper(chosen_name_buffer, timeout_ms);
}
