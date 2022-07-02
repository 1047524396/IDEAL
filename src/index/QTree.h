/*
 * QTree.h
 *
 *  Created on: Jan 1, 2021
 *      Author: teng
 */

#ifndef SRC_INDEX_QTREE_H_
#define SRC_INDEX_QTREE_H_

#include "../include/Pixel.h"
#include <boost/sort/sort.hpp>

enum QT_Direction{
	bottom_left = 0,
	bottom_right = 1,
	top_left = 2,
	top_right = 3
};

class QTNode{

	void internal_leaf_count(int &count){
		if(isleaf()){
			count++;
		}else{
			for(int i=0;i<4;i++){
				children[i]->internal_leaf_count(count);
			}
		}
	}

	void internal_border_leaf_count(int &count){
		if(isleaf()){
			if(!interior&&!exterior){
				count++;
			}
		}else{
			for(int i=0;i<4;i++){
				children[i]->internal_leaf_count(count);
			}
		}
	}

	void internal_distance(Point &p, double &dist, bool geography){
		if(interior || exterior){
			return;
		}
		double tmpd = mbr.distance(p, geography);

		if(!isleaf()){
			for(QTNode *c:children){
				c->internal_distance(p, dist, geography);
			}
		}else if(tmpd<dist){
			dist = tmpd;
		}

	}

public:

	QTNode *children[4] = {NULL, NULL, NULL, NULL};
	box mbr;
	bool interior = false;
	bool exterior = false;
	int level = 0;
	size_t objnum = 0;
	pthread_mutex_t lk;

	QTNode(double low_x, double low_y, double high_x, double high_y){
		pthread_mutex_init(&lk, NULL);
		mbr.low[0] = low_x;
		mbr.low[1] = low_y;
		mbr.high[0] = high_x;
		mbr.high[1] = high_y;
	}
	QTNode(box m){
		pthread_mutex_init(&lk, NULL);
		mbr = m;
	}
	void lock(){
		pthread_mutex_lock(&lk);
	}
	void unlock(){
		pthread_mutex_unlock(&lk);
	}
	bool isleaf(){
		return children[0]==NULL;
	}

	void split(){
		double mid_x = (mbr.high[0]+mbr.low[0])/2;
		double mid_y = (mbr.high[1]+mbr.low[1])/2;
		children[bottom_left] = new QTNode(mbr.low[0],mbr.low[1],mid_x,mid_y);
		children[bottom_right] = new QTNode(mid_x,mbr.low[1],mbr.high[0],mid_y);
		children[top_left] = new QTNode(mbr.low[0],mid_y,mid_x,mbr.high[1]);
		children[top_right] = new QTNode(mid_x,mid_y,mbr.high[0],mbr.high[1]);
		for(int i=0;i<4;i++){
			children[i]->level = level+1;
		}
	}
	void split_to(const size_t target_level){
		if(level==target_level){
			return;
		}
		if(isleaf()){
			split();
		}
		for(int i=0;i<4;i++){
			children[i]->split_to(target_level);
		}
	}
	void push(std::stack<QTNode *> &ws){
		if(!isleaf()){
			for(int i=0;i<4;i++){
				ws.push(children[i]);
			}
		}
	}
	void get_leafs(vector<box *> &leafs){
		if(isleaf()){
			leafs.push_back(new box(mbr));
		}else{
			for(int i=0;i<4;i++){
				children[i]->get_leafs(leafs);
			}
		}
	}
	~QTNode(){
		if(!isleaf()){
			for(int i=0;i<4;i++){
				delete children[i];
				children[i] = NULL;
			}
		}
	}
	int size(){
		// each nodes need 1 byte for node status
		// 2 bytes each for four children
		const int lfc = leaf_count();
		int tmp = lfc;
		int bits = 0;
		while(tmp>0){
			bits++;
			tmp /= 2;
		}
		//int bits = (bits+7)/8*8;
		bits = bits*4+2;
		return (lfc*bits+7)/8;
	}
	int leaf_count(){
		int count = 0;
		internal_leaf_count(count);
		return count;
	}
	int border_leaf_count(){
		int count = 0;
		internal_border_leaf_count(count);
		return count;
	}

//  for queries
	QTNode *retrieve(Point &p){
		assert(mbr.contain(p));
		if(this->isleaf()){
			return this;
		}else{
			int offset =  2*(p.y>(mbr.low[1]+mbr.high[1])/2)+(p.x>(mbr.low[0]+mbr.high[0])/2);
			return children[offset]->retrieve(p);
		}
	}

	void touch(Point &p){
		if(!mbr.contain(p)){
			return;
		}
		if(!isleaf()){
			int offset =  2*(p.y>(mbr.low[1]+mbr.high[1])/2)+(p.x>(mbr.low[0]+mbr.high[0])/2);
			assert(offset<4 && offset>=0);
			children[offset]->touch(p);
		}else{
			lock();
			objnum++;
			unlock();
		}
	}

	size_t merge_objnum(){
		if(!isleaf()){
			//not merged
			assert(objnum==0);
			for(int i=0;i<4;i++){
				objnum += children[i]->merge_objnum();
			}
		}
		return objnum;
	}
	void converge(const size_t threshold){
		if(!isleaf()){
			if(objnum<=threshold){
				// merge the children
				for(int i=0;i<4;i++){
					delete children[i];
					children[i] = NULL;
				}
			}else{
				for(int i=0;i<4;i++){
					children[i]->converge(threshold);
				}
			}
		}
	}


	double distance(Point &p,bool geography){
		double dist = DBL_MAX;
		internal_distance(p,dist,geography);
		return dist;
	}

	bool within(Point &p, double within_dist){
		if(isleaf()&&(interior||exterior)){
			return false;
		}
		if(mbr.distance(p, true)<=within_dist){
			if(isleaf()){
				// boundary pixel
				return true;
			}else{
				// go over children
				for(int i=0;i<4;i++){
					if(children[i]->within(p, within_dist)){
						return true;
					}
				}
			}
		}
		return false;
	}

	void retrieve_within(Point &p, vector<QTNode *> &candidates, double within_dist){
		if(this->mbr.distance(p, true)<=within_dist){
			if(isleaf()){
				if(!interior&&!exterior){
					candidates.push_back(this);
				}
			}else{
				for(int i=0;i<4;i++){
					children[i]->retrieve_within(p, candidates, within_dist);
				}
			}
		}
	}

	// the segment is within the distance
	bool within(Point &start, Point &end, double within_dist){
		if(isleaf()&&(interior||exterior)){
			return false;
		}
		if(mbr.distance(start, end, true)<=within_dist){
			if(isleaf()){
				// boundary pixel
				return true;
			}else{
				// go over children
				for(int i=0;i<4;i++){
					if(children[i]->within(start, end, within_dist)){
						return true;
					}
				}
			}
		}
		return false;
	}

	void retrieve_within(Point &start, Point &end, vector<QTNode *> &candidates, double within_dist){
		if(this->mbr.distance(start, end, true)<=within_dist){
			if(isleaf()){
				if(!interior&&!exterior){
					candidates.push_back(this);
				}
			}else{
				for(int i=0;i<4;i++){
					children[i]->retrieve_within(start, end, candidates, within_dist);
				}
			}
		}
	}


	bool determine_contain(Point &p){
		assert(mbr.contain(p));
		QTNode *n = retrieve(p);
		return n->interior||n->exterior;
	}

	// checking the features of the nodes covered by pixel p
	bool evaluate_nodes(box &p, bool &has_in, bool &has_ex){
		if(!mbr.intersect(p)){
			return true;
		}
		if(isleaf()){
			//border box
			if(!interior&&!exterior){
				return false;
			}
			has_ex |= exterior;
			has_in |= interior;
			//p intersects both internal and external nodes
			if(has_in&&has_ex){
				return false;
			}
		}else{
			for(int i=0;i<4;i++){
				if(!children[i]->evaluate_nodes(p, has_in, has_ex)){
					return false;
				}
			}
		}
		return true;
	}

	bool determine_contain(box &p, bool isin){
		assert(this->level==0);
		bool has_in = false;
		bool has_out = false;
		if(evaluate_nodes(p, has_in, has_out)){
			assert(has_in!=has_out);
			isin = has_in;
			return true;
		}
		return false;
	}
};

#endif /* SRC_INDEX_QTREE_H_ */
