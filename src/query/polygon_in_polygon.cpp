/*
 * Parser.cpp
 *
 *  Created on: May 9, 2020
 *      Author: teng
 */

#include "../geometry/MyPolygon.h"
#include <fstream>
#include "../index/RTree.h"
#include <queue>
#include <boost/program_options.hpp>

namespace po = boost::program_options;
using namespace std;

#define MAX_THREAD_NUM 100

int element_size = 100;

// some shared parameters
pthread_mutex_t poly_lock;

pthread_mutex_t report_lock;
query_context global_ctx;

queue<queue_element *> shared_queue;

RTree<MyPolygon *, double, 2, double> tree;


int source_index = 0;
vector<MyPolygon *> source;
int partition_count = 0;

int batch_num = 100;
void *partition_unit(void *args){
	query_context *ctx = (query_context *)args;
	log("thread %d is started",ctx->thread_id);
	int local_count = 0;
	while(source_index!=source.size()){
		int local_cur = 0;
		int local_end = 0;
		pthread_mutex_lock(&poly_lock);
		if(source_index==source.size()){
			pthread_mutex_unlock(&poly_lock);
			break;
		}
		local_cur = source_index;
		if(local_cur+1>source.size()){
			local_end = source.size();
		}else {
			local_end = local_cur+1;
		}
		source_index = local_end;
		pthread_mutex_unlock(&poly_lock);

		for(int i=local_cur;i<local_end;i++){
			struct timeval start = get_cur_time();

			if(ctx->use_grid){
				source[i]->partition(ctx->vpr);
			}
			if(ctx->use_qtree){
				source[i]->partition_qtree(ctx->vpr);
			}
			double latency = get_time_elapsed(start);
			int num_vertices = source[i]->get_num_vertices();
			ctx->report_latency(num_vertices, latency);
			if(latency>10000||num_vertices>200000){
				logt("partition %d vertices takes",start,num_vertices);
			}
			if(++local_count==1000){
				pthread_mutex_lock(&report_lock);
				partition_count += local_count;
				if(partition_count%10000==0){
					log("partitioned %d polygons",partition_count);
				}
				local_count = 0;
				pthread_mutex_unlock(&report_lock);
			}
		}
	}
	pthread_mutex_lock(&report_lock);
	partition_count += local_count;
	global_ctx = global_ctx + *ctx;
	pthread_mutex_unlock(&report_lock);
	return NULL;
}

bool MySearchCallback(MyPolygon *poly, void* arg){
	query_context *ctx = (query_context *)arg;
	MyPolygon *target = (MyPolygon *)ctx->target;
	// query with parition
	if(ctx->use_grid){
		if(!poly->is_grid_partitioned()){
			poly->partition(ctx->vpr);
		}
		bool isfound = poly->contain_try_partition(target->getMBB(), ctx);

		if(ctx->rastor_only){
			ctx->raster_checked++;
			ctx->found += isfound;
		}else{
			ctx->vector_check++;
			ctx->found += poly->contain(target, ctx);
		}
	}else if(ctx->use_qtree){
		if(!poly->is_grid_partitioned()){
			 poly->partition_qtree(ctx->vpr);
		}
		if(poly->get_qtree()->determine_contain(*(target->getMBB()))){
			ctx->raster_checked++;
		}else{
			ctx->found += poly->contain(target,ctx);
			ctx->vector_check++;
		}
	}else{
		ctx->found += poly->contain(target,ctx);
	}
	// keep going until all hit objects are found
	return true;
}

void *query(void *args){
	query_context *ctx = (query_context *)args;
	pthread_mutex_lock(&poly_lock);
	log("thread %d is started",ctx->thread_id);
	pthread_mutex_unlock(&poly_lock);
	while(shared_queue.size()>0){
		queue_element *elem = NULL;
		pthread_mutex_lock(&poly_lock);
		if(!shared_queue.empty()){
			elem = shared_queue.front();
			shared_queue.pop();
		}
		pthread_mutex_unlock(&poly_lock);
		// queue is empty but not stopped
		if(!elem){
			usleep(10);
			continue;
		}
		for(MyPolygon *poly:elem->polys){
			if(ctx->sample_rate<1.0&&!tryluck(ctx->sample_rate)){
				continue;
			}
			if(poly->get_num_vertices()>100){
				continue;
			}
			ctx->target = (void *)poly;
			Pixel *px = poly->getMBB();
			tree.Search(px->low, px->high, MySearchCallback, (void *)ctx);
			if(++ctx->query_count==1000){
				pthread_mutex_lock(&report_lock);
				global_ctx.query_count += ctx->query_count;
				if(global_ctx.query_count%100000==0){
					log("queried %d polygons",global_ctx.query_count);
				}
				ctx->query_count = 0;
				pthread_mutex_unlock(&report_lock);
			}
		}

		delete elem;
	}
	pthread_mutex_lock(&report_lock);
	global_ctx = global_ctx+*ctx;
	pthread_mutex_unlock(&report_lock);
	return NULL;
}



int main(int argc, char** argv) {
	string source_path;
	string target_path;
	int num_threads = get_num_threads();
	po::options_description desc("query usage");
	desc.add_options()
		("help,h", "produce help message")
		("rasterize,r", "partition with rasterization")
		("qtree,q", "partition with qtree")
		("source,s", po::value<string>(&source_path), "path to the source")
		("target,t", po::value<string>(&target_path), "path to the target")
		("threads,n", po::value<int>(&num_threads), "number of threads")
		("vpr,v", po::value<int>(&global_ctx.vpr), "number of vertices per raster")
		("big_threshold,b", po::value<int>(&global_ctx.big_threshold), "up threshold for complex polygon")
		("small_threshold", po::value<int>(&global_ctx.small_threshold), "low threshold for complex polygon")
		("sample_rate", po::value<float>(&global_ctx.sample_rate), "sample rate")
		;
	po::variables_map vm;
	po::store(po::parse_command_line(argc, argv, desc), vm);
	if (vm.count("help")) {
		cout << desc << "\n";
		return 0;
	}
	po::notify(vm);
	if(!vm.count("source")||!vm.count("target")){
		cout << desc << "\n";
		return 0;
	}

	global_ctx.use_grid = vm.count("rasterize");
	global_ctx.use_qtree = vm.count("qtree");

	pthread_t threads[num_threads];
	query_context ctx[num_threads];
	for(int i=0;i<num_threads;i++){
		ctx[i] = global_ctx;
		ctx[i].thread_id = i;
	}

	timeval start = get_cur_time();

	source = MyPolygon::load_binary_file(source_path.c_str(),global_ctx);
	logt("loaded %ld polygons", start, source.size());

	if(vm.count("rasterize")||vm.count("qtree")){
		for(int i=0;i<num_threads;i++){
			pthread_create(&threads[i], NULL, partition_unit, (void *)&ctx[i]);
		}

		for(int i = 0; i < num_threads; i++ ){
			void *status;
			pthread_join(threads[i], &status);
		}
		logt("partitioned %d polygons", start, partition_count);
	}

	for(MyPolygon *p:source){
		tree.Insert(p->getMBB()->low, p->getMBB()->high, p);
	}
	logt("building R-Tree with %d nodes", start,source.size());

	ifstream is;
	is.open(target_path.c_str(), ios::in | ios::binary);
	int eid = 0;
	queue_element *elem = new queue_element(eid++);
	int loaded = 0;
	size_t pid = 0;
	while(!is.eof()){
		MyPolygon *poly = MyPolygon::read_polygon_binary_file(is);
		if(!poly){
			continue;
		}
		poly->setid(pid++);
		elem->polys.push_back(poly);
		if(elem->polys.size()==element_size){
			shared_queue.push(elem);
			elem = new queue_element(eid++);
		}
		if(++loaded%100000==0){
			log("loaded %d polygons",loaded);
		}
	}
	if(elem->polys.size()>0){
		shared_queue.push(elem);
	}else{
		delete elem;
	}

	logt("loaded %d polygons",start,loaded);
	for(int i=0;i<num_threads;i++){
		pthread_create(&threads[i], NULL, query, (void *)&ctx[i]);
	}

	for(int i = 0; i < num_threads; i++ ){
		void *status;
		pthread_join(threads[i], &status);
	}
	logt("queried %d polygons %ld rastor %ld vector %ld found",start,global_ctx.query_count,global_ctx.raster_checked,global_ctx.vector_check
			,global_ctx.found);

//	for(MyPolygon *p:source){
//		delete p;
//	}
	return 0;
}



