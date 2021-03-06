#include "dump.h"
#include "constants.h"
#include "path.h"
#include "trash.h"
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

struct rrm_dump_info
{
  int next_dump_id;
  int prev_dump_id;
  bool active;
  time_t time;
  size_t files;
  char origin_path[PATH_MAX];
};

static char *rrm_dump_create_path(rrm_trash *trash, int dump_id)
{
  char dump_id_buf[50];
  const char *trash_path;

  sprintf(dump_id_buf, "%i", dump_id);
  trash_path = rrm_trash_get_path(trash);
  return rrm_path_join(trash_path, dump_id_buf);
}

static rrm_status rrm_dump_create_internal(const char *path,
  bool fail_if_exists, const char *origin, int *lock_fd, int *info_fd)
{
  rrm_status status;
  char *lock_path, *info_path, *files_path;
  struct rrm_dump_info info;

  if (mkdir(path, S_IRWXU) < 0) {
    if (errno == EEXIST && fail_if_exists) {
      status = RRM_SAEXST;
      return status;
    }
  }

  lock_path = rrm_path_join(path, RRM_LOCK_FILE_NAME);
  info_path = rrm_path_join(path, RRM_INFO_FILE_NAME);

  *lock_fd = open(lock_path, O_RDWR | O_CREAT, 0666);

  lockf(*lock_fd, F_LOCK, 0);

  files_path = rrm_path_join(path, RRM_FILES_FOLDER_NAME);
  mkdir(files_path, 0666);

  *info_fd = open(info_path, O_RDWR | O_CREAT, 0666);
  info.prev_dump_id = 0;
  info.next_dump_id = 0;
  info.active = true;
  info.time = time(NULL);
  info.files = 0;
  strcpy(info.origin_path, origin);

  pwrite(*info_fd, &info, sizeof(info), 0);

  free(info_path);
  free(lock_path);

  return RRM_SOK;
}

rrm_status rrm_dump_create(struct rrm_trash *trash, int dump_id,
  const char *origin)
{
  rrm_status status;
  char *dump_path;
  int lock_fd, info_fd;

  dump_path = rrm_dump_create_path(trash, dump_id);
  status = rrm_dump_create_internal(dump_path, true, origin, &lock_fd,
    &info_fd);
  if (rrm_status_is_error(status)) {
    return status;
  }

  close(info_fd);
  lockf(lock_fd, F_ULOCK, 0);
  close(lock_fd);

  return RRM_SOK;
}

rrm_status rrm_dump_open(rrm_dump *dump, struct rrm_trash *trash, int dump_id,
  bool create_if_missing, const char *origin)
{
  char *lock_path, *info_path;

  dump->trash = trash;
  dump->path = rrm_dump_create_path(trash, dump_id);
  dump->files_path = rrm_path_join(dump->path, RRM_FILES_FOLDER_NAME);
  dump->dump_id = dump_id;

  if (create_if_missing) {
    return rrm_dump_create_internal(dump->path, false, origin, &dump->lock_fd,
      &dump->info_fd);
  }

  lock_path = rrm_path_join(dump->path, RRM_LOCK_FILE_NAME);
  info_path = rrm_path_join(dump->path, RRM_INFO_FILE_NAME);

  dump->lock_fd = open(lock_path, O_RDWR);
  lockf(dump->lock_fd, F_LOCK, 0);

  dump->info_fd = open(info_path, O_RDWR);

  return RRM_SOK;
}

rrm_status rrm_dump_move_after(rrm_dump *dump, rrm_dump *new_dump)
{
  rrm_status status;
  struct rrm_dump_info info, new_info;

  if (pread(dump->info_fd, &info, sizeof(info), 0) != sizeof(info)) {
    status = rrm_status_from_os(errno);
    goto err_read_info;
  }

  if (pread(new_dump->info_fd, &new_info, sizeof(new_info), 0) !=
      sizeof(new_info)) {
    status = rrm_status_from_os(errno);
    goto err_read_new_info;
  }

  assert(new_info.prev_dump_id == 0);
  assert(new_info.next_dump_id == 0);

  info.next_dump_id = new_dump->dump_id;
  new_info.prev_dump_id = dump->dump_id;

  if (pwrite(dump->info_fd, &info, sizeof(info), 0) != sizeof(info)) {
    status = rrm_status_from_os(errno);
    goto err_write_info;
  }

  if (pwrite(new_dump->info_fd, &new_info, sizeof(new_info), 0) !=
      sizeof(new_info)) {
    status = rrm_status_from_os(errno);
    goto err_write_new_info;
  }

  return RRM_SOK;

err_write_new_info:
  // TODO undo write info?
err_write_info:
err_read_new_info:
err_read_info:
  return status;
}

rrm_status rrm_dump_next(rrm_dump *dump)
{
  rrm_status status;
  rrm_dump next;
  struct rrm_dump_info info;

  if (pread(dump->info_fd, &info, sizeof(info), 0) != sizeof(info)) {
    return rrm_status_from_os(errno);
  }

  if (info.next_dump_id == 0) {
    return RRM_SEND;
  }

  status = rrm_dump_open(&next, dump->trash, info.next_dump_id, false, NULL);
  if (rrm_status_is_error(status)) {
    return status;
  }

  rrm_dump_close(dump);

  *dump = next;

  return RRM_SOK;
}

rrm_status rrm_dump_previous(rrm_dump *dump)
{
  rrm_status status;
  rrm_dump prev;
  struct rrm_dump_info info;

  if (pread(dump->info_fd, &info, sizeof(info), 0) != sizeof(info)) {
    return rrm_status_from_os(errno);
  }

  if (info.next_dump_id == 0) {
    return RRM_SEND;
  }

  rrm_dump_close(dump);

  status = rrm_dump_open(&prev, dump->trash, info.prev_dump_id, false, NULL);
  if (rrm_status_is_error(status)) {
    // TODO iterator invalid
    return status;
  }

  // TODO move right

  *dump = prev;

  return RRM_SOK;
}

int rrm_dump_get_id(const rrm_dump *dump)
{
  return dump->dump_id;
}

time_t rrm_dump_get_time(const rrm_dump *dump)
{
  struct rrm_dump_info info;

  if (pread(dump->info_fd, &info, sizeof(info), 0) != sizeof(info)) {
    return rrm_status_from_os(errno);
  }

  return info.time;
}

rrm_status rrm_dump_get_origin(const rrm_dump *dump, char *output)
{
  struct rrm_dump_info info;

  if (pread(dump->info_fd, &info, sizeof(info), 0) != sizeof(info)) {
    return rrm_status_from_os(errno);
  }

  strcpy(output, info.origin_path);
  return RRM_SOK;
}

static rrm_status rrm_dump_create_file_info(rrm_dump *dump,
  struct rrm_dump_info *info, const char *file)
{
  rrm_status status;
  int file_info_fd;
  char name_buf[50];
  char result[PATH_MAX];
  char *file_info_path;
  size_t file_name_size;

  snprintf(name_buf, sizeof(name_buf), "%zu.info", info->files);
  rrm_path_resolve(info->origin_path, name_buf, sizeof(result), result);

  file_info_path = rrm_path_join(dump->files_path, name_buf); // TODO check NULL

  file_info_fd = open(file_info_path, O_WRONLY | O_CREAT, 0666);
  if (file_info_fd < 0) {
    status = rrm_status_from_os(errno);
    goto err_open;
  }

  file_name_size = strlen(file) + 1;
  if (pwrite(file_info_fd, file, file_name_size, 0) !=
      (ssize_t)file_name_size) {
    status = rrm_status_from_os(errno);
    goto err_write;
  }

  close(file_info_fd);
  free(file_info_path);

  return RRM_SOK;
err_write:
  close(file_info_fd);
err_open:
  free(file_info_path);
  return status;
}

static rrm_status rrm_dump_move_file(rrm_dump *dump, struct rrm_dump_info *info,
  const char *file)
{
  char name_buf[50];
  char result[PATH_MAX];
  char *new_path;

  snprintf(name_buf, sizeof(name_buf), "%zu.data", info->files);
  rrm_path_resolve(info->origin_path, name_buf, sizeof(result), result);

  new_path = rrm_path_join(dump->files_path, name_buf);
  if (rename(file, new_path) == 0) {
    return RRM_SOK;
  }

  free(new_path);
  return rrm_status_from_os(errno);
}

rrm_status rrm_dump_add_file(rrm_dump *dump, const char *file)
{
  rrm_status status;
  struct rrm_dump_info info;

  if (pread(dump->info_fd, &info, sizeof(info), 0) != sizeof(info)) {
    return rrm_status_from_os(errno);
  }

  ++info.files;

  status = rrm_dump_create_file_info(dump, &info, file);
  if (rrm_status_is_error(status)) {
    goto err_create_file_info;
  }

  status = rrm_dump_move_file(dump, &info, file);
  if (rrm_status_is_error(status)) {
    goto err_move_file;
  }

  return rrm_status_from_os(errno);

err_move_file:
  // TODO remove moved file
err_create_file_info:
  return status;
}

void rrm_dump_close(rrm_dump *dump)
{
  close(dump->info_fd);
  lockf(dump->lock_fd, F_ULOCK, 0);
  close(dump->lock_fd);
}
