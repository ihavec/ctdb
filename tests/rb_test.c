/* 
   simple rb test tool

   Copyright (C) Ronnie Sahlberg 2007

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.
   
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   
   You should have received a copy of the GNU General Public License
   along with this program; if not, see <http://www.gnu.org/licenses/>.
*/

#include "includes.h"
#include "lib/events/events.h"
#include "lib/util/dlinklist.h"
#include "system/filesys.h"
#include "popt.h"
#include "cmdline.h"

#include <sys/time.h>
#include <time.h>
#include "common/rb_tree.h"

static struct timeval tp1,tp2;

static void start_timer(void)
{
	gettimeofday(&tp1,NULL);
}

static double end_timer(void)
{
	gettimeofday(&tp2,NULL);
	return (tp2.tv_sec + (tp2.tv_usec*1.0e-6)) - 
		(tp1.tv_sec + (tp1.tv_usec*1.0e-6));
}

int num_records;

void *callback(void *param, void *d)
{
	uint32_t *data = (uint32_t *)d;

	if(!data){
		data = talloc(NULL, uint32_t);
		*data = 0;
	}
	(*data)++;

	return data;
}

void *random_add(void *p, void *d)
{
	if(d){
		talloc_free(d);
	}
	return p;
}

void traverse(void *p, void *d)
{
	uint32_t *data = (uint32_t *)d;

	printf("traverse data:%d\n",*data);
}

void random_traverse(void *p, void *d)
{
	printf("%s   ",(char *)d);
}
	
				
/*
  main program
*/
int main(int argc, const char *argv[])
{
	struct poptOption popt_options[] = {
		POPT_AUTOHELP
		POPT_CTDB_CMDLINE
		{ "num-records", 'r', POPT_ARG_INT, &num_records, 0, "num_records", "integer" },
		POPT_TABLEEND
	};
	int opt;
	const char **extra_argv;
	int extra_argc = 0;
	poptContext pc;
	struct event_context *ev;
	int i,j,k;
	trbt_tree_t *tree;
	uint32_t *data;
	uint32_t key[3];
	uint32_t key1[3] = {0,10,20};
	uint32_t key2[3] = {0,10,21};
	uint32_t key3[3] = {0,11,20};
	uint32_t key4[3] = {2,10,20};

	pc = poptGetContext(argv[0], argc, argv, popt_options, POPT_CONTEXT_KEEP_FIRST);

	while ((opt = poptGetNextOpt(pc)) != -1) {
		switch (opt) {
		default:
			fprintf(stderr, "Invalid option %s: %s\n", 
				poptBadOption(pc, 0), poptStrerror(opt));
			exit(1);
		}
	}

	/* setup the remaining options for the main program to use */
	extra_argv = poptGetArgs(pc);
	if (extra_argv) {
		extra_argv++;
		while (extra_argv[extra_argc]) extra_argc++;
	}

	ev = event_context_init(NULL);


	printf("testing trbt_insert32_callback for %d records\n", num_records);
	tree = trbt_create(NULL);
	for (i=0; i<num_records; i++) {
		trbt_insert32_callback(tree, i, callback, NULL);
	}
	for (i=3; i<num_records; i++) {
		trbt_insert32_callback(tree, i, callback, NULL);
	}

	printf("first 3 keys should have data==1\n");
	printf("the rest of the keys should have data==2\n");
	for (i=0; i<num_records; i++) {
		data = trbt_lookup32(tree, i);
		printf("key:%d data:%d\n", i, *data);
	}


	printf("testing trbt_insertarray32_callback\n");
	tree = trbt_create(NULL);
	trbt_insertarray32_callback(tree, 3, key1, callback, NULL);
	trbt_insertarray32_callback(tree, 3, key1, callback, NULL);
	trbt_insertarray32_callback(tree, 3, key2, callback, NULL);
	trbt_insertarray32_callback(tree, 3, key3, callback, NULL);
	trbt_insertarray32_callback(tree, 3, key2, callback, NULL);
	trbt_insertarray32_callback(tree, 3, key1, callback, NULL);

	data = trbt_lookuparray32(tree, 3, key1);
	printf("key1 dataptr:0x%08x == %d\n",(int)data,data?*data:-1);
	data = trbt_lookuparray32(tree, 3, key2);
	printf("key2 dataptr:0x%08x == %d\n",(int)data,data?*data:-1);
	data = trbt_lookuparray32(tree, 3, key3);
	printf("key3 dataptr:0x%08x == %d\n",(int)data,data?*data:-1);
	data = trbt_lookuparray32(tree, 3, key4);
	printf("key4 dataptr:0x%08x == %d\n",(int)data,data?*data:-1);
	trbt_traversearray32(tree, 3, traverse, NULL);
	
	printf("\ndeleting key4\n");
	trbt_deletearray32(tree, 3, key4);
	data = trbt_lookuparray32(tree, 3, key1);
	printf("key1 dataptr:0x%08x == %d\n",(int)data,data?*data:-1);
	data = trbt_lookuparray32(tree, 3, key2);
	printf("key2 dataptr:0x%08x == %d\n",(int)data,data?*data:-1);
	data = trbt_lookuparray32(tree, 3, key3);
	printf("key3 dataptr:0x%08x == %d\n",(int)data,data?*data:-1);
	data = trbt_lookuparray32(tree, 3, key4);
	printf("key4 dataptr:0x%08x == %d\n",(int)data,data?*data:-1);
	trbt_traversearray32(tree, 3, traverse, NULL);

	printf("\ndeleting key2\n");
	trbt_deletearray32(tree, 3, key2);
	data = trbt_lookuparray32(tree, 3, key1);
	printf("key1 dataptr:0x%08x == %d\n",(int)data,data?*data:-1);
	data = trbt_lookuparray32(tree, 3, key2);
	printf("key2 dataptr:0x%08x == %d\n",(int)data,data?*data:-1);
	data = trbt_lookuparray32(tree, 3, key3);
	printf("key3 dataptr:0x%08x == %d\n",(int)data,data?*data:-1);
	data = trbt_lookuparray32(tree, 3, key4);
	printf("key4 dataptr:0x%08x == %d\n",(int)data,data?*data:-1);
	trbt_traversearray32(tree, 3, traverse, NULL);
	
	printf("\ndeleting key3\n");
	trbt_deletearray32(tree, 3, key3);
	data = trbt_lookuparray32(tree, 3, key1);
	printf("key1 dataptr:0x%08x == %d\n",(int)data,data?*data:-1);
	data = trbt_lookuparray32(tree, 3, key2);
	printf("key2 dataptr:0x%08x == %d\n",(int)data,data?*data:-1);
	data = trbt_lookuparray32(tree, 3, key3);
	printf("key3 dataptr:0x%08x == %d\n",(int)data,data?*data:-1);
	data = trbt_lookuparray32(tree, 3, key4);
	printf("key4 dataptr:0x%08x == %d\n",(int)data,data?*data:-1);
	trbt_traversearray32(tree, 3, traverse, NULL);
	
	printf("\ndeleting key1\n");
	trbt_deletearray32(tree, 3, key1);
	data = trbt_lookuparray32(tree, 3, key1);
	printf("key1 dataptr:0x%08x == %d\n",(int)data,data?*data:-1);
	data = trbt_lookuparray32(tree, 3, key2);
	printf("key2 dataptr:0x%08x == %d\n",(int)data,data?*data:-1);
	data = trbt_lookuparray32(tree, 3, key3);
	printf("key3 dataptr:0x%08x == %d\n",(int)data,data?*data:-1);
	data = trbt_lookuparray32(tree, 3, key4);
	printf("key4 dataptr:0x%08x == %d\n",(int)data,data?*data:-1);
	trbt_traversearray32(tree, 3, traverse, NULL);
	

	printf("\nrun random insert and delete for 60 seconds\n");
	tree = trbt_create(NULL);
	i=0;
	start_timer();
	while(end_timer() < 60.0){
		char *str;

		i++;
		key[0]=random()%10;
		key[1]=random()%10;
		key[2]=random()%10;
		if (random()%2) {
			str=talloc_asprintf(tree, "%d.%d.%d", key[0],key[1],key[2]);
			trbt_insertarray32_callback(tree, 3, key, random_add, str);
		} else {
			trbt_deletearray32(tree, 3, key);
		}
		if(i%1000==999)printf(".");fflush(stdout);
	}
	printf("\n");
	trbt_traversearray32(tree, 3, random_traverse, NULL);
	printf("\n");

	printf("\ndeleting all entries\n");
	for(i=0;i<10;i++){
	for(j=0;j<10;j++){
	for(k=0;k<10;k++){
		key[0]=i;
		key[1]=j;
		key[2]=k;
		trbt_deletearray32(tree, 3, key);
	}
	}
	}
	trbt_traversearray32(tree, 3, random_traverse, NULL);
	printf("\n");
	talloc_report_full(tree, stdout);

	return 0;
}
