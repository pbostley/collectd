/**
 * collectd - src/snmp.c
 * Copyright (C) 2007  Florian octo Forster
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; only version 2 of the License is applicable.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 *
 * Authors:
 *   Florian octo Forster <octo at verplant.org>
 **/

#include "collectd.h"
#include "common.h"
#include "plugin.h"

#include <net-snmp/net-snmp-config.h>
#include <net-snmp/net-snmp-includes.h>

/*
 * Private data structes
 */
struct oid_s
{
  oid oid[MAX_OID_LEN];
  uint32_t oid_len;
};
typedef struct oid_s oid_t;

union instance_u
{
  char  string[DATA_MAX_NAME_LEN];
  oid_t oid;
};
typedef union instance_u instance_t;

struct data_definition_s
{
  char *name; /* used to reference this from the `Collect' option */
  char *type; /* used to find the data_set */
  int is_table;
  instance_t instance;
  oid_t *values;
  int values_len;
  struct data_definition_s *next;
};
typedef struct data_definition_s data_definition_t;

struct host_definition_s
{
  char *name;
  char *address;
  char *community;
  int version;
  struct snmp_session sess;
  data_definition_t **data_list;
  int data_list_len;
  struct host_definition_s *next;
};
typedef struct host_definition_s host_definition_t;

/*
 * Private variables
 */
data_definition_t *data_head = NULL;
host_definition_t *host_head = NULL;

/*
 * Private functions
 */
/* First there are many functions which do configuration stuff. It's a big
 * bloated and messy, I'm afraid. */

/*
 * Callgraph for the config stuff:
 *  csnmp_config
 *  +-> csnmp_config_add_data
 *  !   +-> csnmp_config_add_data_type
 *  !   +-> csnmp_config_add_data_table
 *  !   +-> csnmp_config_add_data_instance
 *  !   +-> csnmp_config_add_data_values
 *  +-> csnmp_config_add_host
 *  +-> csnmp_config_add_collect
 */
static void call_snmp_init_once (void)
{
  static int have_init = 0;

  if (have_init == 0)
    init_snmp (PACKAGE_NAME);
  have_init = 1;
} /* void call_snmp_init_once */

static int csnmp_config_add_data_type (data_definition_t *dd, oconfig_item_t *ci)
{
  if ((ci->values_num != 1) || (ci->values[0].type != OCONFIG_TYPE_STRING))
  {
    WARNING ("snmp plugin: `Type' needs exactly one string argument.");
    return (-1);
  }

  if (dd->type != NULL)
    free (dd->type);

  dd->type = strdup (ci->values[0].value.string);
  if (dd->type == NULL)
    return (-1);

  return (0);
} /* int csnmp_config_add_data_type */

static int csnmp_config_add_data_table (data_definition_t *dd, oconfig_item_t *ci)
{
  if ((ci->values_num != 1) || (ci->values[0].type != OCONFIG_TYPE_BOOLEAN))
  {
    WARNING ("snmp plugin: `Table' needs exactly one boolean argument.");
    return (-1);
  }

  dd->is_table = ci->values[0].value.boolean ? 1 : 0;

  return (0);
} /* int csnmp_config_add_data_table */

static int csnmp_config_add_data_instance (data_definition_t *dd, oconfig_item_t *ci)
{
  if ((ci->values_num != 1) || (ci->values[0].type != OCONFIG_TYPE_STRING))
  {
    WARNING ("snmp plugin: `Instance' needs exactly one string argument.");
    return (-1);
  }

  if (dd->is_table)
  {
    /* Instance is an OID */
    dd->instance.oid.oid_len = MAX_OID_LEN;

    if (!read_objid (ci->values[0].value.string,
	  dd->instance.oid.oid, &dd->instance.oid.oid_len))
    {
      ERROR ("snmp plugin: read_objid (%s) failed.",
	  ci->values[0].value.string);
      return (-1);
    }
  }
  else
  {
    /* Instance is a simple string */
    strncpy (dd->instance.string, ci->values[0].value.string, DATA_MAX_NAME_LEN - 1);
  }

  return (0);
} /* int csnmp_config_add_data_instance */

static int csnmp_config_add_data_values (data_definition_t *dd, oconfig_item_t *ci)
{
  int i;

  if (ci->values_num < 1)
  {
    WARNING ("snmp plugin: `Values' needs at least one argument.");
    return (-1);
  }

  for (i = 0; i < ci->values_num; i++)
    if (ci->values[i].type != OCONFIG_TYPE_STRING)
    {
      WARNING ("snmp plugin: `Values' needs only string argument.");
      return (-1);
    }

  if (dd->values != NULL)
    free (dd->values);
  dd->values = (oid_t *) malloc (sizeof (oid_t) * ci->values_num);
  if (dd->values == NULL)
    return (-1);
  dd->values_len = ci->values_num;

  for (i = 0; i < ci->values_num; i++)
  {
    dd->values[i].oid_len = MAX_OID_LEN;

    if (NULL == snmp_parse_oid (ci->values[i].value.string,
	  dd->values[i].oid, &dd->values[i].oid_len))
    {
      ERROR ("snmp plugin: snmp_parse_oid (%s) failed.",
	  ci->values[i].value.string);
      free (dd->values);
      dd->values = NULL;
      dd->values_len = 0;
      return (-1);
    }
  }

  return (0);
} /* int csnmp_config_add_data_instance */

static int csnmp_config_add_data (oconfig_item_t *ci)
{
  data_definition_t *dd;
  int status = 0;
  int i;

  if ((ci->values_num != 1)
      || (ci->values[0].type != OCONFIG_TYPE_STRING))
  {
    WARNING ("snmp plugin: The `Data' config option needs exactly one string argument.");
    return (-1);
  }

  dd = (data_definition_t *) malloc (sizeof (data_definition_t));
  if (dd == NULL)
    return (-1);
  memset (dd, '\0', sizeof (data_definition_t));

  dd->name = strdup (ci->values[0].value.string);
  if (dd->name == NULL)
  {
    free (dd);
    return (-1);
  }

  for (i = 0; i < ci->children_num; i++)
  {
    oconfig_item_t *option = ci->children + i;
    status = 0;

    if (strcasecmp ("Type", option->key) == 0)
      status = csnmp_config_add_data_type (dd, option);
    else if (strcasecmp ("Table", option->key) == 0)
      status = csnmp_config_add_data_table (dd, option);
    else if (strcasecmp ("Instance", option->key) == 0)
      status = csnmp_config_add_data_instance (dd, option);
    else if (strcasecmp ("Values", option->key) == 0)
      status = csnmp_config_add_data_values (dd, option);
    else
    {
      WARNING ("snmp plugin: Option `%s' not allowed here.", option->key);
      status = -1;
    }

    if (status != 0)
      break;
  } /* for (ci->children) */

  while (status == 0)
  {
    if (dd->type == NULL)
    {
      WARNING ("snmp plugin: `Type' not given for data `%s'", dd->name);
      status = -1;
      break;
    }
    if (dd->values == NULL)
    {
      WARNING ("snmp plugin: No `Value' given for data `%s'", dd->name);
      status = -1;
      break;
    }

    break;
  } /* while (status == 0) */

  if (status != 0)
  {
    sfree (dd->name);
    sfree (dd->values);
    sfree (dd);
    return (-1);
  }

  DEBUG ("snmp plugin: dd = { name = %s, type = %s, is_table = %s, values_len = %i }",
      dd->name, dd->type, (dd->is_table != 0) ? "true" : "false", dd->values_len);

  if (data_head == NULL)
    data_head = dd;
  else
  {
    data_definition_t *last;
    last = data_head;
    while (last->next != NULL)
      last = last->next;
    last->next = dd;
  }

  return (0);
} /* int csnmp_config_add_data */

static int csnmp_config_add_host_address (host_definition_t *hd, oconfig_item_t *ci)
{
  if ((ci->values_num != 1)
      || (ci->values[0].type != OCONFIG_TYPE_STRING))
  {
    WARNING ("snmp plugin: The `Address' config option needs exactly one string argument.");
    return (-1);
  }

  if (hd->address == NULL)
    free (hd->address);

  hd->address = strdup (ci->values[0].value.string);
  if (hd->address == NULL)
    return (-1);

  DEBUG ("snmp plugin: host = %s; host->address = %s;",
      hd->name, hd->address);

  hd->sess.peername = hd->address;

  return (0);
} /* int csnmp_config_add_host_address */

static int csnmp_config_add_host_community (host_definition_t *hd, oconfig_item_t *ci)
{
  if ((ci->values_num != 1)
      || (ci->values[0].type != OCONFIG_TYPE_STRING))
  {
    WARNING ("snmp plugin: The `Community' config option needs exactly one string argument.");
    return (-1);
  }

  if (hd->community == NULL)
    free (hd->community);

  hd->community = strdup (ci->values[0].value.string);
  if (hd->community == NULL)
    return (-1);

  DEBUG ("snmp plugin: host = %s; host->community = %s;",
      hd->name, hd->community);

  hd->sess.community = (u_char *) hd->community;
  hd->sess.community_len = strlen (hd->community);

  return (0);
} /* int csnmp_config_add_host_community */

static int csnmp_config_add_host_version (host_definition_t *hd, oconfig_item_t *ci)
{
  int version;

  if ((ci->values_num != 1)
      || (ci->values[0].type != OCONFIG_TYPE_NUMBER))
  {
    WARNING ("snmp plugin: The `Version' config option needs exactly one number argument.");
    return (-1);
  }

  version = (int) ci->values[0].value.number;
  if ((version != 1) && (version != 2))
  {
    WARNING ("snmp plugin: `Version' must either be `1' or `2'.");
    return (-1);
  }

  hd->version = version;

  if (hd->version == 1)
    hd->sess.version = SNMP_VERSION_1;
  else
    hd->sess.version = SNMP_VERSION_2c;

  return (0);
} /* int csnmp_config_add_host_address */

static int csnmp_config_add_host (oconfig_item_t *ci)
{
  host_definition_t *hd;
  int status = 0;
  int i;

  if ((ci->values_num != 1) || (ci->values[0].type != OCONFIG_TYPE_STRING))
  {
    WARNING ("snmp plugin: `Host' needs exactly one string argument.");
    return (-1);
  }

  hd = (host_definition_t *) malloc (sizeof (host_definition_t));
  if (hd == NULL)
    return (-1);
  memset (hd, '\0', sizeof (host_definition_t));
  hd->version = 2;

  hd->name = strdup (ci->values[0].value.string);
  if (hd->name == NULL)
  {
    free (hd);
    return (-1);
  }

  snmp_sess_init (&hd->sess);
  hd->sess.version = SNMP_VERSION_2c;

  for (i = 0; i < ci->children_num; i++)
  {
    oconfig_item_t *option = ci->children + i;
    status = 0;

    if (strcasecmp ("Address", option->key) == 0)
      status = csnmp_config_add_host_address (hd, option);
    else if (strcasecmp ("Community", option->key) == 0)
      status = csnmp_config_add_host_community (hd, option);
    else if (strcasecmp ("Version", option->key) == 0)
      status = csnmp_config_add_host_version (hd, option);
    else
    {
      WARNING ("snmp plugin: csnmp_config_add_host: Option `%s' not allowed here.", option->key);
      status = -1;
    }

    if (status != 0)
      break;
  } /* for (ci->children) */

  while (status == 0)
  {
    if (hd->address == NULL)
    {
      WARNING ("snmp plugin: `Address' not given for host `%s'", hd->name);
      status = -1;
      break;
    }
    if (hd->community == NULL)
    {
      WARNING ("snmp plugin: `Community' not given for host `%s'", hd->name);
      status = -1;
      break;
    }

    break;
  } /* while (status == 0) */

  if (status != 0)
  {
    sfree (hd->name);
    sfree (hd);
    return (-1);
  }

  /* TODO: Check all fields in `hd'. */
  DEBUG ("snmp plugin: hd = { name = %s, address = %s, community = %s, version = %i }",
      hd->name, hd->address, hd->community, hd->version);

  if (host_head == NULL)
    host_head = hd;
  else
  {
    host_definition_t *last;
    last = host_head;
    while (last->next != NULL)
      last = last->next;
    last->next = hd;
  }

  return (0);
} /* int csnmp_config_add_host */

static int csnmp_config_add_collect (oconfig_item_t *ci)
{
  data_definition_t *data;
  host_definition_t *host;
  data_definition_t **data_list;
  int data_list_len;
  int i;

  if (ci->values_num < 2)
  {
    WARNING ("snmp plugin: `Collect' needs at least two arguments.");
    return (-1);
  }

  for (i = 0; i < ci->values_num; i++)
    if (ci->values[i].type != OCONFIG_TYPE_STRING)
    {
      WARNING ("snmp plugin: All arguments to `Collect' must be strings.");
      return (-1);
    }

  for (host = host_head; host != NULL; host = host->next)
    if (strcasecmp (ci->values[0].value.string, host->name) == 0)
      break;

  if (host == NULL)
  {
    WARNING ("snmp plugin: No such host configured: `%s'",
	ci->values[0].value.string);
    return (-1);
  }

  data_list_len = host->data_list_len + ci->values_num - 1;
  data_list = (data_definition_t **) realloc (host->data_list,
      sizeof (data_definition_t *) * data_list_len);
  if (data_list == NULL)
    return (-1);
  host->data_list = data_list;

  for (i = 1; i < ci->values_num; i++)
  {
    for (data = data_head; data != NULL; data = data->next)
      if (strcasecmp (ci->values[i].value.string, data->name) == 0)
	break;

    if (data == NULL)
    {
      WARNING ("snmp plugin: No such data configured: `%s'",
	  ci->values[i].value.string);
      continue;
    }

    DEBUG ("snmp plugin: Collect: host = %s, data[%i] = %s;",
	host->name, host->data_list_len, data->name);

    host->data_list[host->data_list_len] = data;
    host->data_list_len++;
  } /* for (values_num) */

  return (0);
} /* int csnmp_config_add_collect */

static int csnmp_config (oconfig_item_t *ci)
{
  int i;

  call_snmp_init_once ();

  for (i = 0; i < ci->children_num; i++)
  {
    oconfig_item_t *child = ci->children + i;
    if (strcasecmp ("Data", child->key) == 0)
      csnmp_config_add_data (child);
    else if (strcasecmp ("Host", child->key) == 0)
      csnmp_config_add_host (child);
    else if (strcasecmp ("Collect", child->key) == 0)
      csnmp_config_add_collect (child);
    else
    {
      WARNING ("snmp plugin: Ignoring unknown config option `%s'.", child->key);
    }
  } /* for (ci->children) */

  return (0);
} /* int csnmp_config */

static int csnmp_init (void)
{
  call_snmp_init_once ();
  return (0);
}

#if 0
static void csnmp_submit (gauge_t snum, gauge_t mnum, gauge_t lnum)
{
  value_t values[3];
  value_list_t vl = VALUE_LIST_INIT;

  values[0].gauge = snum;
  values[1].gauge = mnum;
  values[2].gauge = lnum;

  vl.values = values;
  vl.values_len = STATIC_ARRAY_SIZE (values);
  vl.time = time (NULL);
  strcpy (vl.host, hostname_g);
  strcpy (vl.plugin, "load");

  plugin_dispatch_values ("load", &vl);
}
#endif

static value_t csnmp_value_list_to_value (struct variable_list *vl, int type)
{
  value_t ret;
  uint64_t temp = 0;
  int defined = 1;

  if ((vl->type == ASN_INTEGER)
      || (vl->type == ASN_UINTEGER)
      || (vl->type == ASN_COUNTER)
      || (vl->type == ASN_GAUGE))
  {
    temp = *vl->val.integer;
  }
  else if (vl->type == ASN_COUNTER64)
  {
    temp = vl->val.counter64->high;
    temp = temp << 32;
    temp += vl->val.counter64->low;
  }
  else
  {
    WARNING ("snmp plugin: I don't know the ASN type `%i'", (int) vl->type);
    defined = 0;
  }

  if (type == DS_TYPE_COUNTER)
  {
    ret.counter = temp;
  }
  else if (type == DS_TYPE_GAUGE)
  {
    ret.gauge = NAN;
    if (defined != 0)
      ret.gauge = temp;
  }

  return (ret);
} /* value_t csnmp_value_list_to_value */

static int csnmp_read_table (struct snmp_session *sess_ptr,
    host_definition_t *host, data_definition_t *data)
{
  return (0);
} /* int csnmp_read_table */

static int csnmp_read_value (struct snmp_session *sess_ptr,
    host_definition_t *host, data_definition_t *data)
{
  struct snmp_pdu *req;
  struct snmp_pdu *res;
  struct variable_list *vb;

  const data_set_t *ds;
  value_list_t vl = VALUE_LIST_INIT;

  int status;
  int i;

  DEBUG ("snmp plugin: csnmp_read_value (host = %s, data = %s)",
      host->name, data->name);

  ds = plugin_get_ds (data->type);
  if (!ds)
  {
    ERROR ("snmp plugin: DataSet `%s' not defined.", data->type);
    return (-1);
  }

  if (ds->ds_num != data->values_len)
  {
    ERROR ("snmp plugin: DataSet `%s' requires %i values, but config talks about %i",
	data->type, ds->ds_num, data->values_len);
    return (-1);
  }

  vl.values_len = ds->ds_num;
  vl.values = (value_t *) malloc (sizeof (value_t) * vl.values_len);
  if (vl.values == NULL)
    return (-1);
  for (i = 0; i < vl.values_len; i++)
  {
    if (ds->ds[i].type == DS_TYPE_COUNTER)
      vl.values[i].counter = 0;
    else
      vl.values[i].gauge = NAN;
  }

  strncpy (vl.host, host->name, sizeof (vl.host));
  vl.host[sizeof (vl.host) - 1] = '\0';
  strcpy (vl.plugin, "snmp");
  strncpy (vl.type_instance, data->instance.string, sizeof (vl.type_instance));
  vl.type_instance[sizeof (vl.type_instance) - 1] = '\0';

  req = snmp_pdu_create (SNMP_MSG_GET);
  if (req == NULL)
  {
    ERROR ("snmp plugin: snmp_pdu_create failed.");
    sfree (vl.values);
    return (-1);
  }

  for (i = 0; i < data->values_len; i++)
    snmp_add_null_var (req, data->values[i].oid, data->values[i].oid_len);
  status = snmp_synch_response (sess_ptr, req, &res);

  if (status != STAT_SUCCESS)
  {
    ERROR ("snmp plugin: snmp_synch_response failed.");
    sfree (vl.values);
    return (-1);
  }

  vl.time = time (NULL);

  for (vb = res->variables; vb != NULL; vb = vb->next_variable)
  {
    char buffer[1024];
    snprint_variable (buffer, sizeof (buffer),
	vb->name, vb->name_length, vb);
    DEBUG ("snmp plugin: Got this variable: %s", buffer);

    for (i = 0; i < data->values_len; i++)
      if (snmp_oid_compare (data->values[i].oid, data->values[i].oid_len,
	    vb->name, vb->name_length) == 0)
	vl.values[i] = csnmp_value_list_to_value (vb, ds->ds[i].type);
  } /* for (res->variables) */

  snmp_free_pdu (res);

  DEBUG ("snmp plugin: -> plugin_dispatch_values (%s, &vl);", data->type);
  plugin_dispatch_values (data->type, &vl);

  return (0);
} /* int csnmp_read_value */

static int csnmp_read_host (host_definition_t *host)
{
  struct snmp_session *sess_ptr;
  int i;

  DEBUG ("snmp plugin: csnmp_read_host (%s);", host->name);

  sess_ptr = snmp_open (&host->sess);
  if (sess_ptr == NULL)
  {
    snmp_perror ("snmp_open");
    ERROR ("snmp plugin: snmp_open failed.");
    return (-1);
  }

  for (i = 0; i < host->data_list_len; i++)
  {
    data_definition_t *data = host->data_list[i];

    if (data->is_table)
      csnmp_read_table (sess_ptr, host, data);
    else
      csnmp_read_value (sess_ptr, host, data);
  }

  snmp_close (sess_ptr);
  return (0);
} /* int csnmp_read_host */

static int csnmp_read (void)
{
  host_definition_t *host;

  if (host_head == NULL)
  {
    INFO ("snmp plugin: No hosts configured.");
    return (-1);
  }

  for (host = host_head; host != NULL; host = host->next)
    csnmp_read_host (host);

  return (0);
} /* int csnmp_read */

void module_register (void)
{
  plugin_register_complex_config ("snmp", csnmp_config);
  plugin_register_init ("snmp", csnmp_init);
  plugin_register_read ("snmp", csnmp_read);
} /* void module_register */

/*
 * vim: shiftwidth=2 softtabstop=2 tabstop=8
 */
