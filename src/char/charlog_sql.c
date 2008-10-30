// Copyright (c) Athena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

#include "../common/cbasetypes.h"
#include "../common/mmo.h"
#include "../common/socket.h"
#include "../common/sql.h"
#include "../common/strlib.h"
#include <string.h>
#include <stdlib.h> // exit

char   log_db_hostname[32] = "127.0.0.1";
uint16 log_db_port = 3306;
char   log_db_username[32] = "ragnarok";
char   log_db_password[32] = "ragnarok";
char   log_db_database[32] = "log";
char   charlog_table[256] = "charlog";

Sql* sql_handle;
bool enabled = false;


/*=============================================
 * Records an event in the char log
 *---------------------------------------------*/
void char_log(char *fmt, ...)
{
	char esc_username[NAME_LENGTH*2+1];
	char esc_message[255*2+1];
	int retcode;

	if( !enabled )
		return;
}

bool charlog_init(void)
{
	sql_handle = Sql_Malloc();

	if( SQL_ERROR == Sql_Connect(sql_handle, log_db_username, log_db_password, log_db_hostname, log_db_port, log_db_database) )
	{
		Sql_ShowDebug(sql_handle);
		Sql_Free(sql_handle);
		exit(EXIT_FAILURE);
	}

	enabled = true;

	return true;
}

bool charlog_final(void)
{
	Sql_Free(sql_handle);
	sql_handle = NULL;
	return true;
}

bool charlog_config_read(const char* key, const char* value)
{
	if( strcmpi(key, "log_db_ip") == 0 )
		safestrncpy(log_db_hostname, value, sizeof(log_db_hostname));
	else
	if( strcmpi(key, "log_db_port") == 0 )
		log_db_port = (uint16)strtoul(value, NULL, 10);
	else
	if( strcmpi(key, "log_db_id") == 0 )
		safestrncpy(log_db_username, value, sizeof(log_db_username));
	else
	if( strcmpi(key, "log_db_pw") == 0 )
		safestrncpy(log_db_password, value, sizeof(log_db_password));
	else
	if( strcmpi(key, "log_db_db") == 0 )
		safestrncpy(log_db_database, value, sizeof(log_db_database));
	else
	if( strcmpi(key, "charlog_db") == 0 )
		safestrncpy(charlog_table, value, sizeof(charlog_table));
	else
		return false;

	return true;
}