/*
 * partition.cpp
 *
 *  Created on: May 28, 2022
 *      Author: teng
 *
 *
 *  this tool helps to partition the space into N*M tiles based on the distribution of the points
 *
 */

#include <queue>
#include <fstream>
#include <cmath>
#include <vector>
#include <boost/sort/sort.hpp>

#include "../index/hilbert_curve.h"
#include "MyPolygon.h"
#include "query_context.h"
#include "util.h"
#include "../index/QTree.h"
#include "../index/RTree.h"
#include "partition.h"

using namespace std;

const char *partition_type_names[7] = {"str", "slc", "hc", "fg", "qt", "bsp", "bos"};


inline box profileSpace(vector<box *> &geometries){
	box space;
	for(box *p:geometries){
		space.update(*p);
	}
	return space;
}


vector<Tile *> genschema_str(vector<box *> &geometries, size_t cardinality){
	size_t part_num = geometries.size()/cardinality+1;

	size_t dimx = sqrt(part_num);
	size_t dimy = part_num/dimx;
	vector<Tile *> schema;
	struct timeval start = get_cur_time();
	size_t num = geometries.size();
	boost::sort::block_indirect_sort(geometries.begin(), geometries.end(), comparePixelX);

	for(size_t x=0;x<dimx;x++){
		size_t begin = (num/dimx)*x;
		size_t end = (x+1)*(num/dimx);
		if(x==dimx-1){
			end = num;
		}
		boost::sort::block_indirect_sort(geometries.begin()+begin, geometries.begin()+end, comparePixelY);

		size_t cur = begin;
		while(cur<end){
			Tile *b = new Tile();
			for(size_t t = 0;t<cardinality && cur<end; t++, cur++){
				box *obj = geometries[cur];
				b->update(*geometries[cur]);
			}
			// some objects at the right side are also coverd by this slice
			while(cur<num && b->contain(*geometries[cur])){
				cur++;
			}
			schema.push_back(b);
		}
	}
	return schema;
}

vector<Tile *> genschema_slc(vector<box *> &geometries, size_t cardinality){

	vector<Tile *> schema;
	struct timeval start = get_cur_time();
	size_t num = geometries.size();
	boost::sort::block_indirect_sort(geometries.begin(), geometries.end(), comparePixelX);

	size_t cur = 0;
	while(cur<num){
		Tile *b = new Tile();
		for(size_t t = 0;t<cardinality && cur<num; t++, cur++){
			box *obj = geometries[cur];
			b->update(*geometries[cur]);
		}
		// some objects at the right side are also covered by this slice
		while(cur<num && b->contain(*geometries[cur])){
			cur++;
		}
		schema.push_back(b);
	}
	return schema;
}

vector<Tile *> genschema_bos(vector<box *> &geometries, size_t cardinality){
	size_t part_num = geometries.size()/cardinality+1;

	struct timeval start = get_cur_time();
	const size_t objnum = geometries.size();
	vector<Tile *> schema;
	box space = profileSpace(geometries);

	vector<box *> xordered;
	vector<box *> yordered;
	xordered.insert(xordered.begin(), geometries.begin(), geometries.end());
	yordered.insert(yordered.begin(), geometries.begin(), geometries.end());
	boost::sort::block_indirect_sort(xordered.begin(), xordered.end(), comparePixelX);
	boost::sort::block_indirect_sort(yordered.begin(), yordered.end(), comparePixelY);

	logt("sorting",start);

	size_t x_iter = 0;
	size_t y_iter = 0;

	box cursor_box;
	cursor_box.low[0] = cursor_box.high[0] = xordered[x_iter]->low[0];
	cursor_box.low[1] = cursor_box.high[1] = yordered[y_iter]->low[1];

	while(x_iter<objnum && y_iter<objnum){
		box tmp_cursor_box = cursor_box;

		// if cut an X slice
		box xbuffer;
		size_t tmp_x_iter = x_iter;
		for(size_t true_xsize = 0;tmp_x_iter<objnum&&true_xsize<std::min(cardinality, objnum-x_iter);tmp_x_iter++){
			box *obj = xordered[tmp_x_iter];
			// not cut as a Y slice
			if(obj->low[1]>cursor_box.low[1]){
				true_xsize++;
				xbuffer.update(*obj);
				tmp_cursor_box.high[0] = max(tmp_cursor_box.high[0], obj->high[0]);
				tmp_cursor_box.low[0] = obj->low[0];
			}
		}

		// if cut a Y slice
		box ybuffer;
		size_t tmp_y_iter = y_iter;
		for(size_t true_ysize = 0;tmp_y_iter<objnum&&true_ysize<std::min(cardinality, objnum-y_iter);tmp_y_iter++){
			box *obj = yordered[tmp_y_iter];
			// not cut as an X slice
			if(obj->low[0]>cursor_box.low[0]){
				true_ysize++;
				ybuffer.update(*obj);
				tmp_cursor_box.high[1] = max(tmp_cursor_box.high[1], obj->high[1]);
				tmp_cursor_box.low[1] = obj->low[1];
			}
		}

		double xcost = (xbuffer.high[0]-tmp_cursor_box.low[0])/(xbuffer.high[0]-xbuffer.low[0]);
		double ycost = (ybuffer.high[1]-tmp_cursor_box.low[1])/(ybuffer.high[1]-ybuffer.low[1]);

		if(xcost<=ycost){
			cursor_box.low[0] = tmp_cursor_box.low[0];
			cursor_box.high[0] = tmp_cursor_box.high[0];
			x_iter = tmp_x_iter;
			while(x_iter<objnum){
				box *obj = geometries[x_iter];
				if(obj->low[1]>cursor_box.low[1]){
					if(obj->high[0]>xbuffer.high[0]||obj->high[1]>cursor_box.high[1]){
						break;
					}
					xbuffer.update(*obj);
				}
				x_iter++;
			}
			schema.push_back(new Tile(xbuffer));
		}else{
			cursor_box.low[1] = tmp_cursor_box.low[1];
			cursor_box.high[1] = tmp_cursor_box.high[1];
			y_iter = tmp_y_iter;
			while(y_iter<objnum){
				box *obj = geometries[y_iter];
				if(obj->low[0]>cursor_box.low[0]){
					if(obj->high[1]>ybuffer.high[1]||obj->high[0]>cursor_box.high[0]){
						break;
					}
					ybuffer.update(*obj);
				}
				y_iter++;
			}
			schema.push_back(new Tile(ybuffer));
		}
		//log_refresh("%.2f%\%", 100.0*max(x_iter, y_iter)/objnum);
	}
	xordered.clear();
	yordered.clear();
	return schema;
}


vector<Tile *> genschema_hc(vector<box *> &geometries, size_t cardinality){
	assert(geometries.size()>0);

	size_t part_num = geometries.size()/cardinality+1;

	vector<Tile *> schema;

	size_t hcnum = geometries.size()/10;
	size_t hindex = log2(hcnum);
	hindex += (1+(hindex%2==0));
	hcnum = pow(2,hindex);
	size_t dimx = pow(2,hindex/2);
	size_t dimy = pow(2,hindex/2);

	size_t *cell_count = new size_t[hcnum];
	memset((void *)cell_count,0,sizeof(size_t)*hcnum);
	vector<box> cells;
	cells.reserve(hcnum);

	box space = profileSpace(geometries);

	double sx = (space.high[0]-space.low[0])/dimx;
	double sy = (space.high[1]-space.low[1])/dimx;

	for(box *p:geometries){
		size_t x = (p->low[0]-space.low[0])/sx;
		size_t y = (p->low[1]-space.low[1])/sy;
		size_t hc = xy2d(hindex,x,y);
		assert(hc<hcnum);
		if(cell_count[hc]==0){
			cells[hc].low[0] = p->low[0];
			cells[hc].low[1] = p->low[1];
			cells[hc].high[0] = p->high[0];
			cells[hc].high[1] = p->high[1];
		}else{
			cells[hc].update(*p);
		}
		cell_count[hc]++;
	}

	Tile *pix = new Tile();
	size_t cur_num = 0;
	for(size_t i=0;i<hcnum;i++){
		if(cell_count[i]>0){
			pix->update(cells[i]);
			cur_num += cell_count[i];
		}
		if(cur_num>=cardinality){
			schema.push_back(pix);
			pix = new Tile();
			cur_num = 0;
		}
	}
	if(cur_num>0){
		schema.push_back(pix);
	}else{
		delete pix;
	}

	delete []cell_count;
	cells.clear();
	return schema;
}

vector<Tile *> genschema_fg(vector<box *> &geometries, size_t cardinality){

	size_t part_num = geometries.size()/cardinality+1;
	size_t dimx = sqrt(part_num);
	size_t dimy = part_num/dimx;
	vector<Tile *> schema;
	box space = profileSpace(geometries);
	double sx = (space.high[0]-space.low[0])/dimx;
	double sy = (space.high[1]-space.low[1])/dimx;

	for(size_t x=0;x<dimx;x++){
		for(size_t y=0;y<dimy;y++){
			Tile *t = new Tile();
			t->low[0] = space.low[0]+x*sx;
			t->high[0] = space.low[0]+(x+1)*sx;
			t->low[1] = space.low[1]+y*sy;
			t->high[1] = space.low[1]+(y+1)*sy;
			schema.push_back(t);
		}
	}

	return schema;
}

vector<Tile *> genschema_qt(vector<box *> &geometries, size_t cardinality){

	size_t part_num = geometries.size()/cardinality+1;

	vector<Tile *> schema;
	box space = profileSpace(geometries);
	QTNode *qtree = new QTNode(space);

	size_t pnum = std::min(part_num*100, geometries.size());
	size_t max_level = (log2(pnum)/log2(4)+1);

	qtree->split_to(max_level);
	for(box *g:geometries){
		Point p(g->low[0], g->low[1]);
		qtree->touch(p);
	}
	qtree->converge(3*cardinality);

	vector<box *> leafs;
	qtree->get_leafs(leafs);

	for(box *b:leafs){
		schema.push_back(new Tile(*b));
		delete b;
	}
	leafs.clear();

	delete qtree;
	return schema;
}

vector<Tile *> genschema_bsp(vector<box *> &geometries, size_t cardinality){

	vector<Tile *> schema;
	box space = profileSpace(geometries);

	BTNode *btree = new BTNode(space);
	btree->objects.insert(btree->objects.end(), geometries.begin(), geometries.end());
	btree->split_to(cardinality);

	vector<box *> leafs;
	btree->get_leafs(leafs);

	for(box *b:leafs){
		schema.push_back(new Tile(*b));
		delete b;
	}
	leafs.clear();

	delete btree;
	return schema;
}

vector<Tile *> genschema(vector<box *> &geometries, size_t cardinality, PARTITION_TYPE type){
	switch(type){
	case BSP:
		return genschema_bsp(geometries, cardinality);
	case QT:
		return genschema_qt(geometries, cardinality);
	case HC:
		return genschema_hc(geometries, cardinality);
	case BOS:
		return genschema_bos(geometries, cardinality);
	case SLC:
		return genschema_slc(geometries, cardinality);
	case STR:
		return genschema_str(geometries, cardinality);
	case FG:
		return genschema_fg(geometries, cardinality);
	default:
		assert(false && "wrong partitioning type");
	}
	// should never reach here
	return genschema_qt(geometries, cardinality);
}

PARTITION_TYPE parse_partition_type(const char *type){
	for(int i=0;i<7;i++){
		if(strcasecmp(type,partition_type_names[i])==0){
			return (PARTITION_TYPE)i;
		}
	}
	assert(false && "wrong partition type");
	return QT;
}


Tile::Tile(){
	pthread_mutex_init(&lk, NULL);
	id = 0;
}

Tile::Tile(box b){
	pthread_mutex_init(&lk, NULL);
	id = 0;
	low[0] = b.low[0];
	low[1] = b.low[1];
	high[0] = b.high[0];
	high[1] = b.high[1];

}

Tile::~Tile(){
}

void Tile::lock(){
	pthread_mutex_lock(&lk);
}

void Tile::unlock(){
	pthread_mutex_unlock(&lk);
}

bool Tile::insert(box *b, void *obj){
	lock();
	objects.push_back(pair<box *, void *>(b, obj));
	unlock();
	return true;
}

void Tile::build_index(){
	lock();
	for(pair<box *, void *> &p:objects){
		tree.Insert(p.first->low, p.first->high, p.second);
	}
	unlock();
}

bool Tile::lookup_tree(void *obj, void *arg){
	vector<void *> *results = (vector<void *> *)arg;
	results->push_back(obj);
	return true;
}

vector<void *> Tile::lookup(box *b){
	vector<void *> results;
	lock();
	tree.Search(b->low, b->high, lookup_tree, (void *)&results);
	unlock();
	return results;
}

vector<void *> Tile::lookup(Point *p){
	vector<void *> results;
	lock();
	tree.Search((double *)p, (double *)p, lookup_tree, (void *)&results);
	unlock();
	return results;
}

size_t Tile::lookup_count(box *b){
	lock();
	size_t count = tree.Search(b->low, b->high, NULL, NULL);
	unlock();
	return count;
}

size_t Tile::lookup_count(Point *p){
	lock();
	size_t count = tree.Search((double *)p, (double *)p , NULL, NULL);
	unlock();
	return count;
}

void print_tiles(vector<Tile *> &boxes){
	MyMultiPolygon *cboxes = new MyMultiPolygon();
	for(Tile *p:boxes){
		MyPolygon *m = MyPolygon::gen_box(*p);
		cboxes->insert_polygon(m);
	}
	cboxes->print();
	delete cboxes;
}

double skewstdevratio(vector<Tile *> &tiles){
	if(tiles.size()==0){
		return 0;
	}
	size_t total = 0;
	for(Tile *t:tiles){
		total += t->get_objnum();
	}
	double avg = 1.0*total/tiles.size();
	double st = 0.0;
	for(Tile *t:tiles){
		st += (t->get_objnum()-avg)*(t->get_objnum()-avg)/tiles.size();
	}
	return sqrt(st)/avg;
}
