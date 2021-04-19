#include <iostream>
#include <vector>
#include <string>
#include <unordered_map>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <exception>

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
                    throw std::out_of_range("Unknown option \"" + token + "\"");
                args[token] = std::atoi(argv[i++]);
                token.clear();
            }
        }

        uint32_t queue_size(void) const {
            if (args.count("-s"))
                return args.at("-s");
            else
                throw std::out_of_range("Specify size");
        }

        time_t quantum(void) const {
            if (args.count("-q"))
                return args.at("-q");
            else
                throw std::out_of_range("Specify quantum");
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
            q.insert(q.begin(), proc);
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
            q.pop_back();
            count--;
        }

        void pop_arrived(std::vector<Process>& arrived) {
            std::lock_guard<std::recursive_mutex> lock(rmutex);
            if (q.empty())
                return;
            int x = -1, y = -1;
            for (size_t i=0; i<count; i++) {
                if (x >= 0 and y > 0)
                    break;
                if (q.at(i).rem_at <= 0 and x < 0)
                    x = i;
                if (q.at(i).rem_at > 0 and x >= 0 and y < 0)
                    y = i;
            }
            if (x < 0)
                return;
            else if (x == 0 and y < 0) {
                arrived.insert(arrived.end(), q.begin(), q.end());
                q.clear();
                count = 0;
            }
            else {
                std::vector<Process>::iterator it = q.begin();
                y = (y < 0) ? static_cast<int>(count) : y;
                arrived.insert(arrived.end(), it + x, it + y);
                q.erase(it + x, it + y);
                count -= y - x;
            }
            this->pop_arrived(arrived);
        }

        void pop_executed(void) {
            std::lock_guard<std::recursive_mutex> lock(rmutex);
            if (q.empty())
                return;
            int x = -1, y = -1;
            for (size_t i=0; i<count; i++) {
                if (x >= 0 and y > 0)
                    break;
                if (q.at(i).rem_bt <= 0 and x < 0)
                    x = i;
                if (q.at(i).rem_bt > 0 and x >= 0 and y < 0)
                    y = i;
            }
            if (x < 0)
                return;
            else if (x == 0 and y < 0) {
                q.clear();
                count = 0;
            }
            else {
                std::vector<Process>::iterator it = q.begin();
                y = (y < 0) ? static_cast<int>(count) : y;
                q.erase(it + x, it + y);
                count -= y - x;
            }
            this->pop_executed();
        }

        friend void populate_queue(ProcessQueue&);
        friend class WeightedRoundRobin;
};

class WeightedRoundRobin {
    private:
        ProcessQueue& source;
        ProcessQueue arrived;
        time_t quantum;
        std::mutex mutex;

        std::lock_guard<std::mutex>* get_lock(void) {
            return new std::lock_guard<std::mutex>(mutex);
        }

        void process_arrival(void) {
            if (!source.limit or !arrived.limit)
                throw std::logic_error("ProcessQueue(s) have zero capacities");

            std::vector<Process> arrivals;
            arrivals.reserve(source.limit);

            std::lock_guard<std::mutex>* lock = this->get_lock();
            source.pop_arrived(arrivals);
            for (size_t i=arrivals.size(); i>0; i--)
                arrived.push(arrivals.at(i-1));
            delete lock;

            while (true) {
                std::lock_guard<std::mutex>* source_lock = source.get_lock();
                arrivals.clear();
                if (source.size()) {
                    using namespace std::literals::chrono_literals;
                    std::this_thread::sleep_for(1ms);
                }
                else {
                    arrived.wait = false;
                    break;
                }
                for (Process& proc: source.q) {
                    proc.rem_at--;
                    if (!proc.rem_at)
                        std::cout << "PID " << proc.pid << " has arrived\n";
                }
                delete source_lock;

                source.pop_arrived(arrivals);
                std::lock_guard<std::mutex>* lock = this->get_lock();
                for (size_t i=arrivals.size(); i>0; i--)
                    arrived.push(arrivals.at(i-1));
                delete lock;
            }
        }

        void weighted_round_robin(void) {
            size_t i = 0;
            uint32_t time_slice;

            arrived.pop_executed();
            while (arrived.size() or arrived.wait) {
                if (!arrived.size())
                    continue;

                std::lock_guard<std::mutex>* lock = this->get_lock();
                Process& proc = arrived.q.at(i);

                if (proc.rem_bt >= proc.weight * quantum)
                    time_slice = proc.weight * quantum;
                else
                    time_slice = proc.rem_bt;

                std::this_thread::sleep_for(std::chrono::milliseconds(time_slice));
                proc.rem_bt -= time_slice;

                std::cout << "PID " << proc.pid << " executed for " << time_slice << " (ms)\n";
                if (!proc.rem_bt)
                    std::cout << "PID " << proc.pid << " has completed executing\n";

                arrived.pop_executed();
                i = (i >= arrived.size() - 1) ? 0 : i + 1;
                delete lock;
            }
        }

    public:
        WeightedRoundRobin(ProcessQueue& source, time_t quantum):
            source(source), arrived(source.limit), quantum(quantum) {}

        std::thread start(void) {
            return std::thread([this](void) {
                std::thread arrival(&WeightedRoundRobin::process_arrival, this);
                std::thread execution(&WeightedRoundRobin::weighted_round_robin, this);
                arrival.join();
                execution.join();
            });
        }
};

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
        if (weight <= 0)
            throw std::bad_alloc();

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
            if (argc > 1)
                std::cerr << exc.what() << "\n\n";
            std::cerr << "Usage: " << std::string(argv[0]);
            std::cerr << " <OPTION...>\n\n";
            std::cerr << "-s, --size\tsize of the process queue\n";
            std::cerr << "-q, --quantum\ttime quantum (ms) value for round robin scheduling\n";
            return 1;
        }

        ProcessQueue waiting(size);
        populate_queue(waiting);

        WeightedRoundRobin wrr(std::ref(waiting), quantum);
        std::thread weighted_rr = wrr.start();
        weighted_rr.join();
        return 0;
    }
    catch (const std::bad_alloc& exc) {
        std::cerr << "Invalid value(s) assigned\n";
        return 1;
    }
    catch (const std::exception& exc) {
        std::cerr << "Exception(s) caught:\n\n";
        std::cerr << exc.what() << std::endl;
        return 1;
    }
}
