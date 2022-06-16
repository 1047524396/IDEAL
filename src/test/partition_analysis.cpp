/*
 * partition_analysis.cpp
 *
 *  Created on: Jun 8, 2022
 *      Author: teng
 */

#include "partition.h"

namespace po = boost::program_options;

// functions for sampling

template <class O>
void *sample_unit(void *arg){
	query_context *ctx = (query_context *)arg;
	vector<O *> *original = (vector<O *> *)(ctx->target);
	vector<O *> *result = (vector<O *> *)(ctx->target2);
	vector<O *> tmp;
	while(ctx->next_batch(10000)){
		for(size_t i=ctx->index;i<ctx->index_end;i++){
			if(tryluck(ctx->global_ctx->sample_rate)){
				tmp.push_back((*original)[i]);
			}
			ctx->report_progress();
		}
		if(tmp.size()>0){
			ctx->global_ctx->lock();
			result->insert(result->end(), tmp.begin(), tmp.end());
			ctx->global_ctx->unlock();
			tmp.clear();
		}
	}
	ctx->merge_global();
	return NULL;
}

template <class O>
vector<O *> sample(vector<O *> &original, double sample_rate){

	vector<O *> result;
	query_context global_ctx;
	pthread_t threads[global_ctx.num_threads];
	query_context ctx[global_ctx.num_threads];
	global_ctx.target_num = original.size();
	global_ctx.sample_rate = sample_rate;
	global_ctx.report_prefix = "sampling";
	for(int i=0;i<global_ctx.num_threads;i++){
		ctx[i] = global_ctx;
		ctx[i].thread_id = i;
		ctx[i].global_ctx = &global_ctx;
		ctx[i].target = (void *) &original;
		ctx[i].target2 = (void *) &result;
	}
	for(int i=0;i<global_ctx.num_threads;i++){
		pthread_create(&threads[i], NULL, sample_unit<O>, (void *)&ctx[i]);
	}

	for(int i = 0; i < global_ctx.num_threads; i++ ){
		void *status;
		pthread_join(threads[i], &status);
	}

	return result;
}


// functions for partitioning
bool assign(Tile *tile, void *arg){
	tile->insert((box *)arg, (box *)arg);
	return true;
}

void *partition_unit(void *arg){
	query_context *ctx = (query_context *)arg;
	vector<box *> *objects = (vector<box *> *)(ctx->target);
	RTree<Tile *, double, 2, double> *tree = (RTree<Tile *, double, 2, double> *)(ctx->target2);
	while(ctx->next_batch(100)){
		for(size_t i=ctx->index;i<ctx->index_end;i++){
			tree->Search((*objects)[i]->low, (*objects)[i]->high, assign, (void *)((*objects)[i]));
			ctx->report_progress();
		}
	}
	ctx->merge_global();
	return NULL;
}

void partition(vector<box *> &objects, RTree<Tile *, double, 2, double> &tree){

	query_context global_ctx;
	pthread_t threads[global_ctx.num_threads];
	query_context ctx[global_ctx.num_threads];
	global_ctx.target_num = objects.size();
	global_ctx.report_prefix = "partitioning";

	for(int i=0;i<global_ctx.num_threads;i++){
		ctx[i] = global_ctx;
		ctx[i].thread_id = i;
		ctx[i].global_ctx = &global_ctx;
		ctx[i].target = (void *) &objects;
		ctx[i].target2 = (void *) &tree;
	}
	for(int i=0;i<global_ctx.num_threads;i++){
		pthread_create(&threads[i], NULL, partition_unit, (void *)&ctx[i]);
	}

	for(int i = 0; i < global_ctx.num_threads; i++ ){
		void *status;
		pthread_join(threads[i], &status);
	}

}

void *index_unit(void *arg){
	query_context *ctx = (query_context *)arg;
	vector<Tile *> *tiles = (vector<Tile *> *)(ctx->target);
	while(ctx->next_batch(1)){
		for(size_t i=ctx->index;i<ctx->index_end;i++){
			(*tiles)[i]->build_index();
			ctx->report_progress();
		}
	}
	ctx->merge_global();
	return NULL;
}

void indexing(vector<Tile *> &tiles){

	query_context global_ctx;
	pthread_t threads[global_ctx.num_threads];
	query_context ctx[global_ctx.num_threads];
	global_ctx.target_num = tiles.size();
	global_ctx.report_prefix = "indexing";

	for(int i=0;i<global_ctx.num_threads;i++){
		ctx[i] = global_ctx;
		ctx[i].thread_id = i;
		ctx[i].global_ctx = &global_ctx;
		ctx[i].target = (void *) &tiles;
	}
	for(int i=0;i<global_ctx.num_threads;i++){
		pthread_create(&threads[i], NULL, index_unit, (void *)&ctx[i]);
	}

	for(int i = 0; i < global_ctx.num_threads; i++ ){
		void *status;
		pthread_join(threads[i], &status);
	}

}


// function for the query phase
bool search(Tile *tile, void *arg){
	query_context *ctx = (query_context *)arg;
	ctx->found += tile->lookup_count((Point *)ctx->target);
	return true;
}

void *query_unit(void *arg){
	query_context *ctx = (query_context *)arg;
	vector<Point *> *objects = (vector<Point *> *)(ctx->target);
	RTree<Tile *, double, 2, double> *tree= (RTree<Tile *, double, 2, double> *)(ctx->target2);
	while(ctx->next_batch(100)){
		for(size_t i=ctx->index;i<ctx->index_end;i++){
			ctx->target = (void *) (*objects)[i];
			tree->Search((double *)(*objects)[i], (double *)(*objects)[i], search, (void *)ctx);
			ctx->report_progress();
		}
	}
	ctx->merge_global();
	return NULL;
}

size_t query(vector<Point *> &objects, RTree<Tile *, double, 2, double> &tree){

	query_context global_ctx;
	pthread_t threads[global_ctx.num_threads];
	query_context ctx[global_ctx.num_threads];
	global_ctx.target_num = objects.size();
	global_ctx.report_prefix = "querying";

	for(int i=0;i<global_ctx.num_threads;i++){
		ctx[i] = global_ctx;
		ctx[i].thread_id = i;
		ctx[i].global_ctx = &global_ctx;
		ctx[i].target = (void *) &objects;
		ctx[i].target2 = (void *) &tree;
	}
	for(int i=0;i<global_ctx.num_threads;i++){
		pthread_create(&threads[i], NULL, query_unit, (void *)&ctx[i]);
	}

	for(int i = 0; i < global_ctx.num_threads; i++ ){
		void *status;
		pthread_join(threads[i], &status);
	}
	return global_ctx.found;
}

typedef struct{
	double stddev = 0;
	double boundary_rate = 0;
	double found_rate = 0;
	size_t tile_num = 0;
}partition_stat;

partition_stat process(vector<box *> &objects, vector<Point *> &targets, size_t card, PARTITION_TYPE ptype){

	struct timeval start = get_cur_time();
	// generate schema
	vector<Tile *> tiles = genschema(objects, card, ptype);

	RTree<Tile *, double, 2, double> tree;
	for(Tile *t:tiles){
		tree.Insert(t->low, t->high, t);
	}
	logt("%ld tiles are generated with %s partitioning algorithm",start, tiles.size(), partition_type_names[ptype]);

	struct timeval entire_start = get_cur_time();

	// partitioning data
	partition(objects, tree);
	size_t total_num = 0;
	for(Tile *tile:tiles){
		total_num += tile->get_objnum();
	}
	double partition_time = get_time_elapsed(start);
	logt("partitioning data",start);

	// local indexing
	indexing(tiles);
	logt("building local index", start);

	// conduct query
	size_t found = query(targets, tree);
	double query_time = get_time_elapsed(start);
	logt("querying data",start);


	logt("entire process", entire_start);

	partition_stat stat;
	stat.stddev = skewstdevratio(tiles);
	stat.boundary_rate = 1.0*(total_num-objects.size())/objects.size();
	stat.found_rate = 1.0*found/targets.size();
	stat.tile_num = tiles.size();
	// clear the partition schema for this round
	for(Tile *tile:tiles){
		delete tile;
	}
	tiles.clear();

	return stat;
}

int main(int argc, char** argv) {

	string mbr_path;
	string point_path;

	size_t min_cardinality = 2000;
	size_t max_cardinality = 2000;
	bool fixed_cardinality = false;

	double min_sample_rate = 0.01;
	double max_sample_rate = 0.01;

	int sample_rounds = 1;

	string ptype_str = "str";

	po::options_description desc("query usage");
	desc.add_options()
		("help,h", "produce help message")
		("fixed_cardinality,f", "fixed cardinality for all sampling rates")
		("source,s", po::value<string>(&mbr_path)->required(), "path to the source mbrs")
		("target,t", po::value<string>(&point_path)->required(), "path to the target points")

		("partition_type,p", po::value<string>(&ptype_str), "partition type should be one of: str|slc|bos|qt|bsp|hc|fg, if not set, all the algorithms will be tested")

		("sample_rate,r", po::value<double>(&min_sample_rate), "the maximum sample rate (0.01 by default)")
		("max_sample_rate", po::value<double>(&max_sample_rate), "the maximum sample rate (0.01 by default)")

		("cardinality,c", po::value<size_t>(&min_cardinality), "the base number of objects per partition contain (2000 by default)")
		("max_cardinality", po::value<size_t>(&max_cardinality), "the max number of objects per partition contain (2000 by default)")

		("sample_rounds", po::value<int>(&sample_rounds), "the number of rounds the tests should be repeated")

		;
	po::variables_map vm;
	po::store(po::parse_command_line(argc, argv, desc), vm);
	if (vm.count("help")) {
		cout << desc << "\n";
		exit(0);
	}
	po::notify(vm);

	if(max_sample_rate<min_sample_rate){
		max_sample_rate = min_sample_rate;
	}
	if(max_cardinality<min_cardinality){
		max_cardinality = min_cardinality;
	}
	fixed_cardinality = vm.count("fixed_cardinality");

	PARTITION_TYPE start_type = STR;
	PARTITION_TYPE end_type = BSP;
	if(vm.count("partition_type")){
		PARTITION_TYPE ptype = ::parse_partition_type(ptype_str.c_str());
		start_type = ptype;
		end_type = ptype;
	}

	struct timeval start = get_cur_time();
	box *boxes;
	size_t box_num = load_boxes_from_file(mbr_path.c_str(), &boxes);
	vector<box *> box_vec;
	box_vec.resize(box_num);
	for(size_t i=0;i<box_num;i++){
		box_vec[i] = boxes+i;
	}
	logt("%ld objects are loaded",start, box_num);

	Point *points;
	size_t points_num = load_points_from_path(point_path.c_str(), &points);
	vector<Point *> point_vec;
	point_vec.resize(points_num);
	for(size_t i=0;i<points_num;i++){
		point_vec[i] = points+i;
	}
	logt("%ld points are loaded", start, points_num);


	// repeat several rounds for a better estimation
	for(int sr=0;sr<sample_rounds;sr++){
		start = get_cur_time();
		vector<box *> objects = sample<box>(box_vec, max_sample_rate);
		logt("%ld objects are sampled with sample rate %f",start, objects.size(),max_sample_rate);
		vector<Point *> targets = sample<Point>(point_vec, max_sample_rate);
		logt("%ld targets are sampled with sample rate %f",start, targets.size(),max_sample_rate);
		// iterate the sampling rate
		for(double sample_rate = max_sample_rate;sample_rate*1.5>=min_sample_rate; sample_rate /= 2){
			vector<box *> cur_sampled_objects;
			vector<Point *> cur_sampled_points;

			if(sample_rate == max_sample_rate){
				cur_sampled_objects.insert(cur_sampled_objects.begin(), objects.begin(), objects.end());
				cur_sampled_points.insert(cur_sampled_points.begin(), targets.begin(), targets.end());
			}else{
				cur_sampled_objects = sample<box>(objects, sample_rate/max_sample_rate);
				cur_sampled_points = sample<Point>(targets, sample_rate/max_sample_rate);
			}
			logt("resampled %ld MBRs and %ld points with sample rate %f", start,cur_sampled_objects.size(),cur_sampled_points.size(), sample_rate);

			for(int pt=(int)start_type;pt<=(int)end_type;pt++){
				vector<double> num_tiles;
				vector<double> stddevs;
				vector<double> boundary;
				vector<double> found;
				// varying the cardinality
				for(size_t card=min_cardinality; (card/1.5)<=max_cardinality;card*=2){
					size_t real_card = std::max((int)(card*sample_rate), 1);
					PARTITION_TYPE ptype = (PARTITION_TYPE)pt;
					partition_stat stat = process(cur_sampled_objects, cur_sampled_points, real_card, ptype);
					printf("%d,%f,%s,%ld,"
							"%ld,%f,%f,%f\n",
							sr,sample_rate,partition_type_names[ptype], real_card,
							 stat.tile_num, stat.stddev, stat.boundary_rate, stat.found_rate);
					fflush(stdout);
					logt("complete %d\t%f\t%s\t%ld",start,sr,sample_rate,partition_type_names[ptype], real_card);
					num_tiles.push_back(stat.tile_num);
					stddevs.push_back(stat.stddev);
					boundary.push_back(stat.boundary_rate);
					found.push_back(stat.found_rate);
				}
				if(num_tiles.size()>1){
					double a,b;
					linear_regression(num_tiles,stddevs,a,b);
					printf("%f,%.10f,%.10f",sample_rate,a,b);
					linear_regression(num_tiles,boundary,a,b);
					printf(",%.10f,%.10f",a,b);
					linear_regression(num_tiles,found,a,b);
					printf(",%.10f,%.10f\n",a,b);
				}
			}
			cur_sampled_objects.clear();
			cur_sampled_points.clear();
		}
		objects.clear();
		targets.clear();
	}

	delete []boxes;
	delete []points;
	return 0;
}
