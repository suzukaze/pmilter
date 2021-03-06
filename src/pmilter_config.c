/*
** pmilter - A Programmable Mail Filter
**
** See Copyright Notice in LICENSE
*/

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sysexits.h>
#include <unistd.h>

#include "libmilter/mfapi.h"
#include "libmilter/mfdef.h"

#include "toml.h"
#include "toml_private.h"

#include "mruby.h"
#include "mruby/compile.h"
#include "mruby/string.h"

#include "pmilter.h"
#include "pmilter_log.h"

#define PMILTER_GET_HANDLER_CONFIG_VALUE(root, node, config, phase)                                                    \
  node = toml_get(root, "handler.mruby_" #phase "_handler");                                                           \
  if (node != NULL) {                                                                                                  \
    config->mruby_##phase##_handler_path = node->value.string;                                                         \
  } else {                                                                                                             \
    config->mruby_##phase##_handler_path = NULL;                                                                       \
  }

pmilter_config *pmilter_config_init()
{
  pmilter_config *config;

  /* need free */
  config = malloc(sizeof(pmilter_config));
  if (config == NULL) {
    return NULL;
  }

  config->log_level = PMILTER_LOG_WARN;
  config->enable_mruby_handler = 0;

  return config;
}

void command_rec_free(command_rec *cmd)
{
  if (cmd->envelope_from != NULL) {
    free(cmd->envelope_from);
  }
  if (cmd->helohost != NULL) {
    free(cmd->helohost);
  }

  /* connecntion_rec free */
  if (cmd->conn->ipaddr != NULL) {
    free(cmd->conn->ipaddr);
  }
  if (cmd->conn->hostname != NULL) {
    free(cmd->conn->hostname);
  }
  free(cmd->conn);
  free(cmd->header);

  free(cmd);
}

#define pmilter_mruby_code_free(code)                                                                                  \
  if (code != PMILTER_CONF_UNSET)                                                                                      \
  free(code)

void pmilter_mrb_delete_conf(pmilter_state *pmilter)
{

  command_rec_free(pmilter->cmd);

  pmilter_mruby_code_free(pmilter->mruby_connect_handler);
  pmilter_mruby_code_free(pmilter->mruby_helo_handler);
  pmilter_mruby_code_free(pmilter->mruby_envfrom_handler);
  pmilter_mruby_code_free(pmilter->mruby_envrcpt_handler);
  pmilter_mruby_code_free(pmilter->mruby_header_handler);
  pmilter_mruby_code_free(pmilter->mruby_eoh_handler);
  pmilter_mruby_code_free(pmilter->mruby_body_handler);
  pmilter_mruby_code_free(pmilter->mruby_eom_handler);
  pmilter_mruby_code_free(pmilter->mruby_abort_handler);
  pmilter_mruby_code_free(pmilter->mruby_close_handler);
  pmilter_mruby_code_free(pmilter->mruby_unknown_handler);
  pmilter_mruby_code_free(pmilter->mruby_data_handler);

  mrb_close(pmilter->mrb);

  free(pmilter);
}

pmilter_state *pmilter_mrb_create_conf(pmilter_config *config)
{
  pmilter_state *pmilter;

  /* need free */
  pmilter = malloc(sizeof(pmilter_state));
  if (pmilter == NULL) {
    return NULL;
  }

  pmilter->config = config;

  pmilter->mruby_connect_handler = PMILTER_CONF_UNSET;
  pmilter->mruby_helo_handler = PMILTER_CONF_UNSET;
  pmilter->mruby_envfrom_handler = PMILTER_CONF_UNSET;
  pmilter->mruby_envrcpt_handler = PMILTER_CONF_UNSET;
  pmilter->mruby_header_handler = PMILTER_CONF_UNSET;
  pmilter->mruby_eoh_handler = PMILTER_CONF_UNSET;
  pmilter->mruby_body_handler = PMILTER_CONF_UNSET;
  pmilter->mruby_eom_handler = PMILTER_CONF_UNSET;
  pmilter->mruby_abort_handler = PMILTER_CONF_UNSET;
  pmilter->mruby_close_handler = PMILTER_CONF_UNSET;
  pmilter->mruby_unknown_handler = PMILTER_CONF_UNSET;
  pmilter->mruby_data_handler = PMILTER_CONF_UNSET;

  if (config->enable_mruby_handler) {
    pmilter->mrb = mrb_open();
    if (pmilter->mrb == NULL) {
      return NULL;
    }

    pmilter_mrb_class_init(pmilter->mrb);
  } else {
    pmilter->mrb = NULL;
  }

  return pmilter;
}

struct toml_node *mrb_pmilter_config_init(const char *path)
{
  struct toml_node *root;
  char *buf = "[foo]\nbar = 'fuga'\n";
  size_t len = sizeof(buf);

  /* TODO: file open */
  toml_init(&root);
  toml_parse(root, buf, len);

  return root;
}

void mrb_pmilter_config_free(struct toml_node *root)
{
  toml_free(root);
}

int pmilter_config_get_integer(pmilter_config *config, struct toml_node *root, char *key)
{
  struct toml_node *node = toml_get(root, key);

  if (!toml_type(node) == TOML_INT) {
    pmilter_log_error(PMILTER_LOG_EMERG, config, "%s must be integer type in config", key);
    exit(1);
  }

  return node->value.integer;
}

char *pmilter_config_get_string(pmilter_config *config, struct toml_node *root, char *key)
{
  struct toml_node *node = toml_get(root, key);

  if (!toml_type(node) == TOML_STRING) {
    pmilter_log_error(PMILTER_LOG_EMERG, config, "%s must be string type in config", key);
    exit(1);
  }

  return node->value.string;
}

int pmilter_config_get_bool(pmilter_config *config, struct toml_node *root, char *key)
{
  struct toml_node *node = toml_get(root, key);

  if (!toml_type(node) == TOML_BOOLEAN) {
    pmilter_log_error(PMILTER_LOG_EMERG, config, "%s must be boolen type in config", key);
    exit(1);
  }

  if (node->value.integer) {
    return 1;
  }

  return 0;
}

int pmilter_config_get_log_level(struct toml_node *root)
{
  int i;
  int log_level = PMILTER_LOG_WARN;
  struct toml_node *node = toml_get(root, "server.log_level");

  if (node != NULL) {
    log_level = pmilter_get_log_level(node->value.string);
  }

  return log_level;
}

void pmilter_config_parse(pmilter_config *config, struct toml_node *root)
{
  struct toml_node *node;

  config->log_level = pmilter_config_get_log_level(root);
  config->enable_mruby_handler = pmilter_config_get_bool(config, root, "server.mruby_handler");
  config->timeout = pmilter_config_get_integer(config, root, "server.timeout");
  config->listen = pmilter_config_get_string(config, root, "server.listen");
  config->listen_backlog = pmilter_config_get_integer(config, root, "server.listen_backlog");
  config->debug = pmilter_config_get_integer(config, root, "server.debug");

  PMILTER_GET_HANDLER_CONFIG_VALUE(root, node, config, connect);
  PMILTER_GET_HANDLER_CONFIG_VALUE(root, node, config, helo);
  PMILTER_GET_HANDLER_CONFIG_VALUE(root, node, config, envfrom);
  PMILTER_GET_HANDLER_CONFIG_VALUE(root, node, config, envrcpt);
  PMILTER_GET_HANDLER_CONFIG_VALUE(root, node, config, header);
  PMILTER_GET_HANDLER_CONFIG_VALUE(root, node, config, eoh);
  PMILTER_GET_HANDLER_CONFIG_VALUE(root, node, config, body);
  PMILTER_GET_HANDLER_CONFIG_VALUE(root, node, config, eom);
  PMILTER_GET_HANDLER_CONFIG_VALUE(root, node, config, abort);
  PMILTER_GET_HANDLER_CONFIG_VALUE(root, node, config, close);
  PMILTER_GET_HANDLER_CONFIG_VALUE(root, node, config, unknown);
  PMILTER_GET_HANDLER_CONFIG_VALUE(root, node, config, data);
}

void usage(char *prog)
{
  fprintf(stderr, "Usage: %s -p socket-addr -c config.toml [-t timeout]\n", prog);
}

struct toml_node *pmilter_config_load(char *file, char **argv)
{
  struct stat st;
  struct toml_node *toml_root;
  void *toml_content = NULL;
  int fd, ret, toml_content_size = 0;

  if (!file) {
    fprintf(stderr, "%s: Missing required -c argument\n", argv[0]);
    usage(argv[0]);
    exit(EX_USAGE);
  }

  fd = open(file, O_RDONLY);
  if (fd == -1) {
    fprintf(stderr, "open: %s\n", strerror(errno));
    exit(1);
  }

  ret = fstat(fd, &st);
  if (ret == -1) {
    fprintf(stderr, "stat: %s\n", strerror(errno));
    exit(EXIT_FAILURE);
  }

  toml_content = mmap(NULL, st.st_size, PROT_READ, MAP_FILE | MAP_PRIVATE, fd, 0);
  if (!toml_content) {
    fprintf(stderr, "mmap: %s\n", strerror(errno));
    exit(EXIT_FAILURE);
  }

  toml_content_size = st.st_size;

  ret = toml_init(&toml_root);
  if (ret == -1) {
    fprintf(stderr, "toml_init: %s\n", strerror(errno));
    exit(EXIT_FAILURE);
  }

  ret = toml_parse(toml_root, toml_content, toml_content_size);
  if (ret) {
    fprintf(stderr, "toml parse failed: %s\n", strerror(errno));
    exit(EXIT_FAILURE);
  }

  ret = munmap(toml_content, toml_content_size);
  if (ret) {
    fprintf(stderr, "munmap: %s\n", strerror(errno));
    exit(EXIT_FAILURE);
  }

  close(fd);

  return toml_root;
}
