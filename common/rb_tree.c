/* 
   a talloc based red-black tree

   Copyright (C) Ronnie Sahlberg  2007

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
#include "rb_tree.h"

#define NO_MEMORY_FATAL(p) do { if (!(p)) { \
          DEBUG(0,("Out of memory for %s at %s\n", #p, __location__)); \
	  exit(10); \
	  }} while (0)


trbt_tree_t *
trbt_create(TALLOC_CTX *memctx)
{
	trbt_tree_t *tree;

	tree = talloc_zero(memctx, trbt_tree_t);
	NO_MEMORY_FATAL(tree);

	return tree;
}

static inline trbt_node_t *
trbt_parent(trbt_node_t *node)
{
	return node->parent;
}

static inline trbt_node_t *
trbt_grandparent(trbt_node_t *node)
{
	trbt_node_t *parent;

	parent=trbt_parent(node);
	if(parent){
		return parent->parent;
	}
	return NULL;
}

static inline trbt_node_t *
trbt_uncle(trbt_node_t *node)
{
	trbt_node_t *parent, *grandparent;

	parent=trbt_parent(node);
	if(!parent){
		return NULL;
	}
	grandparent=trbt_parent(parent);
	if(!grandparent){
		return NULL;
	}
	if(parent==grandparent->left){
		return grandparent->right;
	}
	return grandparent->left;
}


static inline void trbt_insert_case1(trbt_tree_t *tree, trbt_node_t *node);
static inline void trbt_insert_case2(trbt_tree_t *tree, trbt_node_t *node);

static inline void
trbt_rotate_left(trbt_node_t *node)
{
	trbt_tree_t *tree = node->tree;

	if(node->parent){
		if(node->parent->left==node){
			node->parent->left=node->right;
		} else {
			node->parent->right=node->right;
		}
	} else {
		tree->tree=node->right;
	}
	node->right->parent=node->parent;
	node->parent=node->right;
	node->right=node->right->left;
	if(node->right){
		node->right->parent=node;
	}
	node->parent->left=node;
}

static inline void
trbt_rotate_right(trbt_node_t *node)
{
	trbt_tree_t *tree = node->tree;

	if(node->parent){
		if(node->parent->left==node){
			node->parent->left=node->left;
		} else {
			node->parent->right=node->left;
		}
	} else {
		tree->tree=node->left;
	}
	node->left->parent=node->parent;
	node->parent=node->left;
	node->left=node->left->right;
	if(node->left){
		node->left->parent=node;
	}
	node->parent->right=node;
}

static inline void
trbt_insert_case5(trbt_tree_t *tree, trbt_node_t *node)
{
	trbt_node_t *grandparent;
	trbt_node_t *parent;

	parent=trbt_parent(node);
	grandparent=trbt_parent(parent);
	parent->rb_color=TRBT_BLACK;
	grandparent->rb_color=TRBT_RED;
	if( (node==parent->left) && (parent==grandparent->left) ){
		trbt_rotate_right(grandparent);
	} else {
		trbt_rotate_left(grandparent);
	}
}

static inline void
trbt_insert_case4(trbt_tree_t *tree, trbt_node_t *node)
{
	trbt_node_t *grandparent;
	trbt_node_t *parent;

	parent=trbt_parent(node);
	grandparent=trbt_parent(parent);
	if(!grandparent){
		return;
	}
	if( (node==parent->right) && (parent==grandparent->left) ){
		trbt_rotate_left(parent);
		node=node->left;
	} else if( (node==parent->left) && (parent==grandparent->right) ){
		trbt_rotate_right(parent);
		node=node->right;
	}
	trbt_insert_case5(tree, node);
}

static inline void
trbt_insert_case3(trbt_tree_t *tree, trbt_node_t *node)
{
	trbt_node_t *grandparent;
	trbt_node_t *parent;
	trbt_node_t *uncle;

	uncle=trbt_uncle(node);
	if(uncle && (uncle->rb_color==TRBT_RED)){
		parent=trbt_parent(node);
		parent->rb_color=TRBT_BLACK;
		uncle->rb_color=TRBT_BLACK;
		grandparent=trbt_grandparent(node);
		grandparent->rb_color=TRBT_RED;
		trbt_insert_case1(tree, grandparent);
	} else {
		trbt_insert_case4(tree, node);
	}
}

static inline void
trbt_insert_case2(trbt_tree_t *tree, trbt_node_t *node)
{
	trbt_node_t *parent;

	parent=trbt_parent(node);
	/* parent is always non-NULL here */
	if(parent->rb_color==TRBT_BLACK){
		return;
	}
	trbt_insert_case3(tree, node);
}

static inline void
trbt_insert_case1(trbt_tree_t *tree, trbt_node_t *node)
{
	trbt_node_t *parent;

	parent=trbt_parent(node);
	if(!parent){
		node->rb_color=TRBT_BLACK;
		return;
	}
	trbt_insert_case2(tree, node);
}

static inline trbt_node_t *
trbt_sibling(trbt_node_t *node)
{
	trbt_node_t *parent;

	parent=trbt_parent(node);
	if(!parent){
		return NULL;
	}

	if (node == parent->left) {
		return parent->right;
	} else {
		return parent->left;
	}
}

static inline trbt_node_t *
trbt_sibline(trbt_node_t *node)
{
	if (node==node->parent->left) {
		return node->parent->right;
	} else {
		return node->parent->left;
	}
}

static inline void
trbt_delete_case6(trbt_node_t *node)
{
	trbt_node_t *sibling, *parent;

	sibling = trbt_sibling(node);
	parent  = trbt_parent(node);

	sibling->rb_color = parent->rb_color;
	parent->rb_color = TRBT_BLACK;
	if (node == parent->left) {
		sibling->right->rb_color = TRBT_BLACK;
		trbt_rotate_left(parent);
	} else {
		sibling->left->rb_color = TRBT_BLACK;
		trbt_rotate_right(parent);
	}
}


static inline void
trbt_delete_case5(trbt_node_t *node)
{
	trbt_node_t *parent, *sibling;

	parent = trbt_parent(node);
	sibling = trbt_sibling(node);
	if ( (node == parent->left)
	   &&(sibling->rb_color == TRBT_BLACK)
	   &&(sibling->left->rb_color == TRBT_RED)
	   &&(sibling->right->rb_color == TRBT_BLACK) ){
		sibling->rb_color = TRBT_RED;
		sibling->left->rb_color = TRBT_BLACK;
		trbt_rotate_right(sibling);
		trbt_delete_case6(node);
		return;
	}
	if ( (node == parent->right)
	   &&(sibling->rb_color == TRBT_BLACK)
	   &&(sibling->right->rb_color == TRBT_RED)
	   &&(sibling->left->rb_color == TRBT_BLACK) ){
		sibling->rb_color = TRBT_RED;
		sibling->right->rb_color = TRBT_BLACK;
		trbt_rotate_left(sibling);
		trbt_delete_case6(node);
		return;
	}

	trbt_delete_case6(node);
}

static inline void
trbt_delete_case4(trbt_node_t *node)
{
	trbt_node_t *sibling;

	sibling = trbt_sibling(node);
	if ( (node->parent->rb_color == TRBT_RED)
	   &&(sibling->rb_color == TRBT_BLACK)
	   &&(sibling->left->rb_color == TRBT_BLACK)
	   &&(sibling->right->rb_color == TRBT_BLACK) ){
		sibling->rb_color = TRBT_RED;
		node->parent->rb_color = TRBT_BLACK;
	} else {
		trbt_delete_case5(node);
	}
}

static void trbt_delete_case1(trbt_node_t *node);

static inline void
trbt_delete_case3(trbt_node_t *node)
{
	trbt_node_t *sibling;

	sibling = trbt_sibling(node);
	if ( (node->parent->rb_color == TRBT_BLACK)
	   &&(sibling->rb_color == TRBT_BLACK)
	   &&(sibling->left->rb_color == TRBT_BLACK)
	   &&(sibling->right->rb_color == TRBT_BLACK) ){
		sibling->rb_color = TRBT_RED;
		trbt_delete_case1(node->parent);
	} else {
		trbt_delete_case4(node);
	}
}
	
static inline void
trbt_delete_case2(trbt_node_t *node)
{
	trbt_node_t *sibling;

	sibling = trbt_sibling(node);
	if (sibling->rb_color == TRBT_RED) {
		if (node == node->parent->left) {
			trbt_rotate_left(node->parent);
		} else {
			trbt_rotate_right(node->parent);
		}
	}
	trbt_delete_case3(node);
}	

static void
trbt_delete_case1(trbt_node_t *node)
{
	if (!node->parent) {
		return;
	} else {
		trbt_delete_case2(node);
	}
}

static void
delete_node(trbt_node_t *node)
{
	trbt_node_t *child, *parent;

	if (node->left != NULL && node->right != NULL) {
		/* This node has two children, just copy the data */
		/* find the predecessor */
		trbt_node_t *temp = node->left;

		while (temp->right != NULL) {
			temp = temp->right;
		}

		/* swap the predecessor data and key with the node to
		   be deleted.
		 */
		talloc_free(node->data);
		node->data  = talloc_steal(node, temp->data);
		node->key32 = temp->key32;
		temp->data  = NULL;
		temp->key32 = -1;
		/* then delete the temp node.
		   this node is guaranteed to have at least one leaf child */
		delete_node(temp);
		return;
	}

	/* There is at most one child to this node to be deleted */
	child = node->left;
	if (node->right) {
		child = node->right;
	}

	/* replace node with child */
	parent=trbt_parent(node);
	if (parent) {
		if (parent->left == node) {
			parent->left = child;
		} else {
			parent->right = child;
		}
	} else {
		node->tree->tree = child;
	}

	if (child) {
		child->parent = node->parent;

		if (node->rb_color == TRBT_BLACK) {
			if (child->rb_color == TRBT_RED) {
				child->rb_color = TRBT_BLACK;
			} else {
				trbt_delete_case1(child);
			}
		}
	}
	
	talloc_free(node);
	return;
}

static inline trbt_node_t *
trbt_create_node(trbt_tree_t *tree, trbt_node_t *parent, uint32_t key, void *data)
{
	trbt_node_t *node;

	node=talloc_zero(tree, trbt_node_t);
	NO_MEMORY_FATAL(node);

	node->tree=tree;
	node->rb_color=TRBT_BLACK;
	node->parent=parent;
	node->left=NULL;
	node->right=NULL;
	node->key32=key;
	node->data=talloc_steal(node, data);

	return node;
}

/* insert a new node in the tree. 
   if there is already a node with a matching key in the tree 
   we reurn an error
 */
int
trbt_insert32(trbt_tree_t *tree, uint32_t key, void *data)
{
	trbt_node_t *node;

	node=tree->tree;

	/* is this the first node ?*/
	if(!node){
		node = trbt_create_node(tree, NULL, key, data);

		tree->tree=node;
		return 0;
	}

	/* it was not the new root so walk the tree until we find where to
	 * insert this new leaf.
	 */
	while(1){
		/* this node already exists, so just return an error */
		if(key==node->key32){
			return -1;
		}
		if(key<node->key32) {
			if(!node->left){
				/* new node to the left */
				trbt_node_t *new_node;

				new_node = trbt_create_node(tree, node, key, data);
				node->left=new_node;
				node=new_node;

				break;
			}
			node=node->left;
			continue;
		}
		if(key>node->key32) {
			if(!node->right){
				/* new node to the right */
				trbt_node_t *new_node;

				new_node = trbt_create_node(tree, node, key, data);
				node->right=new_node;
				node=new_node;
				break;
			}
			node=node->right;
			continue;
		}
	}

	/* node will now point to the newly created node */
	node->rb_color=TRBT_RED;
	trbt_insert_case1(tree, node);
	return 0;
}

void *
trbt_lookup32(trbt_tree_t *tree, uint32_t key)
{
	trbt_node_t *node;

	node=tree->tree;

	while(node){
		if(key==node->key32){
			return node->data;
		}
		if(key<node->key32){
			node=node->left;
			continue;
		}
		if(key>node->key32){
			node=node->right;
			continue;
		}
	}
	return NULL;
}

void 
trbt_delete32(trbt_tree_t *tree, uint32_t key)
{
	trbt_node_t *node;

	node=tree->tree;

	while(node){
		if(key==node->key32){
			delete_node(node);
			return;
		}
		if(key<node->key32){
			node=node->left;
			continue;
		}
		if(key>node->key32){
			node=node->right;
			continue;
		}
	}
}



# if 0
static void printtree(trbt_node_t *node, int levels)
{
	int i;
	if(node==NULL)return;
	printtree(node->left, levels+1);

	for(i=0;i<levels;i++)printf("    ");
	printf("key:%d COLOR:%s\n",node->key32,node->rb_color==TRBT_BLACK?"BLACK":"RED");

	printtree(node->right, levels+1);
	printf("\n");
}

void print_tree(trbt_tree_t *tree)
{
	printtree(tree->tree->left, 1);
	printf("root node key:%d COLOR:%s\n",tree->tree->key32,tree->tree->rb_color==TRBT_BLACK?"BLACK":"RED");
	printtree(tree->tree->right, 1);
}


#include "../common/rb_tree.h"
void 
test_tree(void)
{
	trbt_tree_t *tree;
	char *str;
	int i, ret;

#define NUM 10
	tree=trbt_create(talloc_new(NULL));
	printf("tree:0x%08x  %d nodes\n",(int)tree,NUM);
	for(i=0;i<NUM;i++){
		str=talloc_asprintf(tree, "STRING#%d", i);
		ret=trbt_insert32(tree, i, str);
		printf("%s ret:%d\n",str, ret);
	}
	print_tree(tree);
	for(i=0;i<NUM;i++){
		str=trbt_lookup32(tree, i);
		printf("lookedup i:%d str:%s\n",i,str);
	}
	trbt_delete32(tree, 9);
	print_tree(tree);
	for(i=0;i<NUM;i++){
		str=trbt_lookup32(tree, i);
		printf("lookedup i:%d str:%s\n",i,str);
	}
}

#endif