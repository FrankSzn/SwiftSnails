#include <iostream>
#include "../../utils/all.h"
#include "../../core/system/node_init.h"
#include "../../core/system/worker/init_config.h"
#include "../../core/parameter/global_pull_access.h"
#include "../../core/parameter/global_push_access.h"
#include "access_method.h"

using namespace swift_snails;
using namespace std;
using namespace fms;
using namespace logistic_regression;


struct Record {
    typedef index_t key_t;
    typedef float val_t;
    typedef float grad_t;

    float target;
    std::vector< std::pair<key_t, val_t> > feas;
};  // struct Record


class Algorithm {

public:

    typedef index_t key_t;
    typedef float val_t;
    typedef float grad_t;
    typedef pair<key_t, val_t> rcd_t;

    explicit Algorithm() {
        _path = global_config().get_config("data_path").to_string();
        _num_iters = global_config().get_config("num_iters").to_int32();
        learning_rate = global_config().get_config("learning_rate").to_float();
        CHECK(_path.empty() ) << "data_path config need to be inited";
        // init async channel
        int _async_channel_thread_num = global_config().get_config("async_channel_thread_num").to_int32();
        CHECK_GT(_async_channel_thread_num, 0);
        AsynExec as(_async_channel_thread_num);
        _async_channel = as.open();
    }


    void operator() () {
        init_local_param_keys(_async_channel_thread_num);
        first_pull_to_init_local_param();
    }

    // batch train
    void train() {
        for(int i = 0; i < _num_iters; i ++) {
            LOG(WARNING) << i << " th iteration";
            train_iter(_async_channel_thread_num);
        }
    }

    void init_local_param_keys(int thread_num) {
        LOG(WARNING) << "init local parameter cache";
        DLOG(INFO) << "start " << thread_num << " threads to gather keys";
        CHECK_GT(thread_num, 0);
        // make sure the following task wait for the init period
        FILE* file = std::fopen(_path.c_str(), "r");
        LineFileReader line_reader;
        std::mutex file_mut;

        std::set<key_t> keys;

        std::function<void(const std::string& line)> handle_line \
            = [this, &keys] (const std::string& line) {
                auto rcd = parse_record(line);
                for(auto &item : rcd.feas) {
                    keys.emplace(item.first);
                }
            };

        AsynExec::task_t task = [file, &file_mut, &handle_line] {
            auto _handle_line = handle_line;
            scan_file_by_line(file, file_mut, std::move(_handle_line) );
        };

        async_exec(thread_num, std::move(task), async_channel());
        std::fclose(file);
        // get num of features
        for(auto& key : keys) {
            if(key > num_feas) num_feas = key;
        }
        num_feas ++;
    }

    // should init local parameter cache's keys
    void first_pull_to_init_local_param() {
        StateBarrier barrier;
        voidf_t extra_rsp_callback = [&barrier] {
            barrier.set_state_valid();
            barrier.try_unblock();
        };
        pull_access.pull();
        barrier.block();
    }

protected:
    /*
     * learn the records in one line
     */
    void learn_one_record(Record &&record) {
        auto& params = param_cache.params();
        auto& grads = param_cache.grads();
        // prepare parameter vector 
        Vec param = Vec::Zero(num_feas);
        for(int i = 0; i < num_feas; i++) {
            param[i] = params[i];
        }

        // prepare rcd
        Vec fea = Vec::Zero(num_feas);
        for(auto& r : record.feas) {
            fea[r.first] = r.second;
        }

        // calculate
        float dot_ = param.dot(fea);
        float h = 1.0 / (1.0 + exp( 1.0 / dot_));

        Vec grad = (record.target - h) * fea;

        // update local parameter
        for(int i = 0; i < num_feas; i ++) {
            grads[i] += learning_rate * grad[i];
        }
    }
    // one iteration
    void train_iter(int thread_num) {

        LOG(INFO) << "train file with " << thread_num << " threads";
        FILE* file = fopen(_path.c_str(), "r");
        LineFileReader line_reader;
        std::mutex file_mut;

        std::function<void(const std::string& line)> handle_line \
            = [this] (const std::string& line) {
                auto rcds = parse_record(line);
                learn_one_record(std::move(rcds));
            };

        AsynExec::task_t task = [file, &file_mut, &handle_line] {
            auto _handle_line = handle_line;
            scan_file_by_line(file, file_mut, std::move(_handle_line) );
        };

        async_exec(thread_num, std::move(task), async_channel());
        std::fclose(file);
    }

    // parse record with target
    Record parse_record(const string &line) {
        Record rcd;

        int i = 0;
        for(const auto& item : split(line, " ")) {
            if(i++ == 0) {
                char *end;
                rcd.target = strtof(item.c_str(), &end);
                continue;
            }
            std::pair<string, string> kv = key_value_split(item, ":");
            key_t key = stoi(kv.first);
            val_t val = stof(kv.second);
            
            rcd.feas.emplace_back(std::move(key), std::move(val));
        }
        return std::move(rcd);
    }


protected:
    std::shared_ptr<AsynExec::channel_t>& async_channel() {
        return _async_channel;
    }

private:
    string _path;
    std::shared_ptr<AsynExec::channel_t> _async_channel;

    typedef GlobalPullAccess<key_t, val_t, grad_t> pull_access_t;
    typedef GlobalPushAccess<key_t, val_t, grad_t> push_access_t;
    typedef GlobalParamCache<key_t, val_t, grad_t> param_cache_t;

    param_cache_t &param_cache = global_param_cache<key_t, val_t, grad_t>();

    pull_access_t& pull_access = global_pull_access<key_t, val_t, grad_t>();
    push_access_t& push_access = global_push_access<key_t, val_t, grad_t>();
    
    int _async_channel_thread_num = 0;
    int num_feas = 0;
    int _num_iters = 0;
    float learning_rate = 0.01;
};

int main(int argc, char* argv[]) {
    // init config
    CMDLine cmdline(argc, argv);
    string param_config_path = cmdline.registerParameter("config", "path of config file");
    string param_help = cmdline.registerParameter("help", "this screen");
    // parse parameters
    if(cmdline.hasParameter(param_help) || argc == 1) {
        cout << endl;
        cout << "===================================================================" << endl;
        cout << "master server main program";
        cout << "===================================================================" << endl;
        cmdline.print_help();
        cout << endl;
        cout << endl;
        return 0;
    }
    if(!cmdline.hasParameter(param_config_path)) {
        LOG(ERROR) << "missing parameter: config";
        return 0;
    }
    std::string config_path = cmdline.getValue(param_config_path);

    worker_init_configs();

    global_config().load_conf(config_path);
    global_config().parse();

    NodeTransferInit node_transfer_init;
    NodeHashfragInit node_hashfrag_init;


    node_transfer_init(false);
    node_hashfrag_init();
    
    return 0;
};