#include "manager.h"
#include "parse.h"
#include "utils.h"
#include <stdlib.h>
#include <stdio.h>

void parse_test(char *spec)
{
	int i;
	mongo_con_manager *manager;
	mongo_servers *servers;
	char *error_message;

	manager = mongo_init();
	manager->log_function = mongo_log_printf;

	servers = mongo_parse_init();
	if (mongo_parse_server_spec(manager, servers, spec, (char **) &error_message)) {
		printf("error_message: %s\n", error_message);
		free(error_message);
	}

	for (i = 0; i < servers->count; i++) {
		char *tmp_hash;

		tmp_hash = mongo_server_create_hash(servers->server[i]);
		printf("HASH: %s\n", tmp_hash);
		free(tmp_hash);
	}
	mongo_servers_dump(manager, servers);

	mongo_servers_dtor(servers);
	mongo_deinit(manager);
}

int main(void)
{
	parse_test("host1:123");
	parse_test("host1:123,host2:123");
	parse_test("mongodb://host1:123,host2:123");
	parse_test("mongodb://derick:test@host1:123");
	parse_test("mongodb://derick:test@host1:123,host2:123");
	parse_test("mongodb://derick:test@host1:123,host2:123/database");
	parse_test("mongodb://derick:test@host1:123,host2/database");
	parse_test("mongodb://derick:test@host1,host2:123/database");
	parse_test("mongodb://host1,host2:123/database");
	/* Specifying options */
	parse_test("mongodb://derick:test@host1,host2:123/database?option1=foo");
	parse_test("mongodb://derick:test@host1,host2:123/?option2=bar");
	parse_test("mongodb://derick:test@host1,host2:123?option3=bar");
	parse_test("mongodb://derick:test@host1,host2:123/?option4");
	parse_test("mongodb://derick:test@host1,host2:123/?option5=bar;option6=baz");
	parse_test("mongodb://derick:test@host1,host2:123/?option7=bar&option8=baz");
	parse_test("mongodb://derick:test@host1,host2:123/?option9=bar;option10");
	parse_test("mongodb://derick:test@host1,host2:123/?option11;option12=baz");
	/* Specific options */
	parse_test("mongodb://host1/?replicaSet=testset");

	return 0;
}
