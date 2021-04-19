#include <iostream>
#include <vector>
#include <string>
#include <unordered_map>
#include <thread>
#include <mutex>
#include <future>
#include <exception>

class ProcessQueue;
class TimedLog;
class WeightedRoundRobin;

class InputParser {
    private:
        std::unordered_map<std::string, uint32_t> args;

    public:
        InputParser(const int& argc, char* argv[]) {
            size_t i = 1;
            std::string token;

            while (i < argc) {
                token = std::string(argv[i++]);
                if (token == "--size")
                    token = std::string("-s");
                else if (token == "-s") {}
                else if (token == "--quantum")
                    token = std::string("-q");
                else if (token == "-q") {}
                else
                    throw std::out_of_range("unknown option");
                args[token] = std::atoi(argv[i++]);
            }
        }

        uint32_t queue_size(void) const {
            if (args.count("-s"))
                return args.at("-s");
            else
                throw std::out_of_range("key out of range");
        }

        time_t quantum(void) const {
            if (args.count("-q"))
                return args.at("-q");
            else
                throw std::out_of_range("key out of range");
        }
};

class Process {
    public:
        volatile const uint32_t pid;
        volatile const uint32_t arrival_time;
        volatile const uint32_t burst_time;
        volatile const uint32_t weight;
        uint32_t rem_at;
        uint32_t rem_bt;

        Process(uint32_t pid, uint32_t at, uint32_t bt, uint32_t weight):
            pid(pid), arrival_time(at), burst_time(bt), weight(weight) {
            rem_at = at;
            rem_bt = bt;
        }

        void operator= (const Process& proc) {
            *(uint32_t*)(&pid) = proc.pid;
            *(uint32_t*)(&arrival_time) = proc.arrival_time;
            *(uint32_t*)(&burst_time) = proc.burst_time;
            *(uint32_t*)(&weight) = proc.weight;
            rem_at = proc.rem_at;
            rem_bt = proc.rem_bt;
        }
};

class ProcessQueue {
    private:
        const size_t limit;
        std::atomic<size_t> count;
        std::atomic<bool> wait;
        std::mutex mutex;
        std::recursive_mutex rmutex;
        std::vector<Process> q;

    public:
        ProcessQueue(size_t limit):
            limit(limit) {
            count = 0;
            wait = true;
            q.reserve(limit);
        }

        std::lock_guard<std::mutex>* get_lock(void) {
            return new std::lock_guard<std::mutex>(mutex);
        }

        size_t size(void) {
            std::lock_guard<std::mutex> lock(mutex);
            return count;
        }

        void push(Process proc) {
            std::lock_guard<std::mutex> lock(mutex);
            if (count >= limit)
                throw std::overflow_error("queue is full");
            for (size_t i=count; i>0; i--)
                q.at(i) = q.at(i-1);
            q.at(0) = proc;
            count++;
        }

        Process front(void) {
            std::lock_guard<std::mutex> lock(mutex);
            if (count <= 0)
                throw std::underflow_error("queue is empty");
            return q.at(count-1);
        }

        void pop(void) {
            std::lock_guard<std::mutex> lock(mutex);
            if (count <= 0)
                throw std::underflow_error("queue is empty");
            count--;
        }

        void pop_arrived(std::vector<Process>& arrived) {
            std::lock_guard<std::recursive_mutex> lock(rmutex);
            int x = -1, y = -1;
            for (size_t i=0; i<count; i++) {
                if (x > 0 and y > 0)
                    break;
                if (q.at(i).rem_at <= 0 and x < 0)
                    x = i;
                if (q.at(i).rem_at > 0 and x > 0 and y < 0)
                    y = i;
            }
            if (x < 0)
                return;
            else if (x == 0 and y < 0) {
                for (size_t i=0; i<count; i++)
                    arrived.push_back(q.at(i));
                count = 0;
            }
            else {
                for (size_t i=y; i<count; i++) {
                    if (i - y + x < y)
                        arrived.push_back(q.at(i));
                    q.at(i - y + x) = q.at(i);
                }
                count -= y - x;
            }
            this->pop_arrived(arrived);
        }

        void pop_executed(void) {
            std::lock_guard<std::recursive_mutex> lock(rmutex);
            int x = -1, y = -1;
            for (size_t i=0; i<count; i++) {
                if (x > 0 and y > 0)
                    break;
                if (q.at(i).rem_bt <= 0 and x < 0)
                    x = i;
                if (q.at(i).rem_bt > 0 and x > 0 and y < 0)
                    y = i;
            }
            if (x < 0)
                return;
            else if (x == 0 and y < 0)
                count = 0;
            else {
                std::vector<Process>::iterator it = q.begin();
                q.erase(it + x, it + y);
                count -= y - x;
            }
            this->pop_executed();
        }

        friend void populate_queue(ProcessQueue&);
        friend class WeightedRoundRobin;
};

class TimedLog {
    public:
        const std::string text;
        const uint32_t pause;

        TimedLog(std::string text, uint32_t pause):
            text(text), pause(pause) {}
};

class WeightedRoundRobin {
    private:
        ProcessQueue& source;
        ProcessQueue arrived;
        time_t quantum;
        std::mutex mutex;

        void process_arrival(void) {
            std::lock_guard<std::mutex> lock(mutex);
            if (!source.limit or !arrived.limit)
                throw std::logic_error("ProcessQueue(s) have zero capacities");

            std::vector<Process> arrivals;
            arrivals.reserve(source.limit);

            source.pop_arrived(arrivals);
            for (size_t i=arrivals.size(); i>0; i--)
                arrived.push(arrivals.at(i-1));

            while (true) {
                arrivals.clear();
                if (source.size()) {
                    using namespace std::literals::chrono_literals;
                    std::this_thread::sleep_for(1ms);
                }
                else {
                    arrived.wait = false;
                    break;
                }
                std::lock_guard<std::mutex>* lock = source.get_lock();
                for (Process& proc: source.q)
                    proc.rem_at--;
                delete lock;
                source.pop_arrived(arrivals);
                for (size_t i=arrivals.size(); i>0; i--)
                    arrived.push(arrivals.at(i-1));
            }
        }

        void weighted_round_robin(std::promise<const std::vector<TimedLog>&>& promise) {
            std::lock_guard<std::mutex> lock(mutex);
            static std::vector<TimedLog> logs;
            size_t i = 0;
            uint32_t time_slice;
            std::string log;

            arrived.pop_executed();
            while (arrived.size() or arrived.wait) {
                std::lock_guard<std::mutex>* lock = arrived.get_lock();
                Process& proc = arrived.q.at(i);

                if (proc.rem_bt >= proc.weight * quantum)
                    time_slice = proc.weight * quantum;
                else
                    time_slice = proc.rem_bt;

                std::this_thread::sleep_for(std::chrono::milliseconds(time_slice));
                proc.rem_bt -= time_slice;

                log = "PID " + std::to_string(proc.pid) + " executed for " + std::to_string(time_slice) + " (ms)";
                logs.push_back(TimedLog(log, time_slice));
                if (!proc.rem_bt) {
                    log.clear();
                    log = "PID " + std::to_string(proc.pid) + " has completed executing";
                    logs.push_back(TimedLog(log, 0));
                }
                delete lock;

                log.clear();
                arrived.pop_executed();
                i = (i > arrived.size() - 1) ? 0 : i + 1;
            }

            promise.set_value(std::cref(logs));
        }

        void print_logs(const std::vector<TimedLog>& logs) {
            std::lock_guard<std::mutex> lock(mutex);
            for (const TimedLog& log: logs) {
                std::this_thread::sleep_for(std::chrono::milliseconds(log.pause));
                std::cout << log.text << std::endl;
            }
        }

    public:
        WeightedRoundRobin(ProcessQueue& source, time_t quantum):
            source(source), arrived(source.limit), quantum(quantum) {}

        std::thread start(void) {
            return std::thread([this](void) {
                std::promise<const std::vector<TimedLog>&> promise;
                std::future<const std::vector<TimedLog>&> future = promise.get_future();
                    
                std::thread arrival(&WeightedRoundRobin::process_arrival, this);
                std::thread execution(&WeightedRoundRobin::weighted_round_robin, this, std::ref(promise));
                std::thread logging(&WeightedRoundRobin::print_logs, this, std::cref(future.get()));

                arrival.join();
                execution.join();
                logging.join();
            });
        }
};

void print_logs(const std::vector<TimedLog>& logs) {
    for (const TimedLog& log: logs) {
        std::this_thread::sleep_for(std::chrono::milliseconds(log.pause));
        std::cout << log.text << std::endl;
    }
}

void populate_queue(ProcessQueue& pq) {
    if (!pq.limit)
        throw std::logic_error("ProcessQueue has zero capacity");

    std::cout << "Enter arrival time, burst time (ms) and weight (Press e to stop)\n";
    std::cout << "================================\n";

    uint32_t at, bt, weight;
    size_t count = 0;
    char keystroke;

    for (size_t i=1;; i++) {
        if (count >= pq.limit) {
            pq.wait = false;
            break;
        }

        std::cout << "PID " << i << ": ";
        keystroke = std::getchar();

        if (keystroke == 'e') {
            pq.wait = false;
            break;
        }
        else
            std::cin.putback(keystroke);

        std::cin >> at >> bt >> weight;
        pq.push(Process(i, at, bt, weight));
        std::cin.sync();
        count++;
    }
}

int main(int argc, char* argv[]) {
    try {
        uint32_t size;
        time_t quantum;

        try {
            InputParser args(std::cref(argc), argv);
            size = args.queue_size();
            quantum = args.quantum();
        }
        catch (const std::out_of_range& exc) {
            std::cerr << "Usage: " << std::string(argv[0]);
            std::cerr << " <OPTION...>\n\n";
            std::cerr << "-s, --size\tsize of the process queue\n";
            std::cerr << "-q, --quantum\ttime quantum (ms) value for round robin scheduling\n";
            exit(1);
        }

        ProcessQueue waiting(size);
        populate_queue(waiting);

        WeightedRoundRobin wrr(std::ref(waiting), quantum);
        std::thread weighted_rr = wrr.start();
        weighted_rr.join();
        return 0;
    }
    catch (const std::exception& exc) {
        std::cerr << exc.what() << std::endl;
        exit(1);
    }
}
