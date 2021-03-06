#include <iostream>
#include <vector>
#include <string>
#include <unordered_map>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <limits>
#include <exception>

class InputParser {
    private:
        std::unordered_map<std::string, uint32_t> args;
        std::unordered_map<std::string, float> rates;

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
                else if (token == "--rate-arrived") {
                    token = std::string("-rn");
                    float rate = std::atof(argv[i++]);
                    if (rate < 0)
                        throw std::bad_alloc();
                    rates[token] = rate;
                    continue;
                }
                else if (token == "-rn") {
                    float rate = std::atof(argv[i++]);
                    if (rate < 0)
                        throw std::bad_alloc();
                    rates[token] = rate;
                    continue;
                }
                else if (token == "--rate-accepted") {
                    token = std::string("-ra");
                    float rate = std::atof(argv[i++]);
                    if (rate < 0)
                        throw std::bad_alloc();
                    rates[token] = rate;
                    continue;
                }
                else if (token == "-ra") {
                    float rate = std::atof(argv[i++]);
                    if (rate < 0)
                        throw std::bad_alloc();
                    rates[token] = rate;
                    continue;
                }
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

        float rate_new(void) const {
            if (rates.count("-rn"))
                return rates.at("-rn");
            else
                throw std::out_of_range("Specify rate-arrived");
        }

        float rate_accepted(void) const {
            if (rates.count("-ra"))
                return rates.at("-ra");
            else
                throw std::out_of_range("Specify rate-accepted");
        }
};

class Process {
    public:
        volatile const uint32_t pid;
        volatile const uint32_t arrival_time;
        volatile const uint32_t burst_time;
        uint32_t rem_bt;
        uint32_t rem_at;
        float priority;

        Process(uint32_t pid, uint32_t at, uint32_t bt):
            pid(pid), arrival_time(at), burst_time(bt) {
            rem_at = at;
            rem_bt = bt;
            priority = 0;
        }

        void operator= (const Process& proc) {
            *(uint32_t*)(&pid) = proc.pid;
            *(uint32_t*)(&arrival_time) = proc.arrival_time;
            *(uint32_t*)(&burst_time) = proc.burst_time;
            rem_at = proc.rem_at;
            rem_bt = proc.rem_bt;
            priority = proc.priority;
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

        float threshold(void) {
            std::lock_guard<std::mutex> lock(mutex);
            if (!count)
                return 0;

            float minp = std::numeric_limits<float>::max();
            for (const Process& proc: q) {
                if (minp > proc.priority)
                    minp = proc.priority;
            }
            return minp;
        }

        friend void populate_queue(ProcessQueue&);
        friend class SelfishRoundRobin;
};

class SelfishRoundRobin {
    private:
        ProcessQueue& source;
        ProcessQueue arrived;
        ProcessQueue accepted;
        time_t quantum;
        std::mutex mutex;

        void process_arrival(void) {
            std::lock_guard<std::mutex> lock(mutex);
            if (!source.limit or !arrived.limit)
                throw std::logic_error("ProcessQueue(s) have zero capacitites");
            
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
                for (Process& proc: source.q) {
                    proc.rem_at--;
                    if (!proc.rem_at)
                        std::cout << "PID " << proc.pid << " has arrived\n";
                }
                delete lock;
                source.pop_arrived(arrivals);
                for (size_t i=arrivals.size(); i>0; i--)
                    arrived.push(arrivals.at(i));
            }
        }

        void accept_arrived(float rn) {
            std::lock_guard<std::mutex> lock(mutex);
            using namespace std::literals::chrono_literals;
            using iter = std::vector<Process>::iterator;

            while (arrived.size() or arrived.wait) {
                std::this_thread::sleep_for(1ms);
                std::lock_guard<std::mutex>* lock = arrived.get_lock();
                for (Process& proc: arrived.q)
                    proc.priority += rn;
                for (iter it = arrived.q.begin(); it != arrived.q.end(); it++) {
                    try {
                        Process proc = *it;
                        if (proc.priority >= accepted.threshold()) {
                            accepted.push(proc);
                            arrived.q.erase(it);
                            arrived.count--;
                            std::cout << "PID " << proc.pid << " has been accepted\n";
                        }
                    }
                    catch (const std::overflow_error& exc) {
                        continue;
                    }
                }
                delete lock;
            }
            
            accepted.wait = false;
        }

        void selfish_round_robin(float ra) {
            std::lock_guard<std::mutex> lock(mutex);
            size_t i = 0;

            accepted.pop_executed();
            while (accepted.size() or accepted.wait) {
                std::lock_guard<std::mutex>* lock = accepted.get_lock();
                Process& proc = accepted.q.at(i);

                std::this_thread::sleep_for(std::chrono::milliseconds(quantum));
                proc.rem_bt -= quantum;
                
                for (Process& proc: accepted.q)
                    proc.priority += ra;

                std::cout << "PID " << proc.pid << " executed for " << quantum << " (ms)\n";
                if (!proc.rem_bt)
                    std::cout << "PID " << proc.pid << " has completed executing\n";

                delete lock;
                accepted.pop_executed();
                i = (i > accepted.size() - 1) ? 0 : i + 1;
            }
        }

    public:
        SelfishRoundRobin(ProcessQueue& source, time_t quantum):
            source(source), arrived(source.limit), accepted(source.limit), quantum(quantum) {}

        std::thread start(float rn, float ra) {
            return std::thread([this, rn, ra](void) {
                std::thread arrival(&SelfishRoundRobin::process_arrival, this);
                std::thread acceptance(&SelfishRoundRobin::accept_arrived, this, rn);
                std::thread execution(&SelfishRoundRobin::selfish_round_robin, this, ra);
                arrival.join();
                acceptance.join();
                execution.join();
            });
        }
};

void populate_queue(ProcessQueue& pq) {
    if (!pq.limit)
        throw std::logic_error("ProcessQueue has zero capacity");
    
    std::cout << "Enter arrival time and burst time (ms) (Press e to stop)\n";
    std::cout << "======================================\n";

    uint32_t at, bt;
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
        
        std::cin >> at >> bt;
        pq.push(Process(i, at, bt));
        std::cin.sync();
        count++;
    }
}

int main(int argc, char* argv[]) {
    try {
        uint32_t size;
        time_t quantum;
        float rn, ra;

        try {
            InputParser args(std::cref(argc), argv);
            size = args.queue_size();
            quantum = args.quantum();
            rn = args.rate_new();
            ra = args.rate_accepted();
        }
        catch (const std::bad_alloc& exc) {
            std::cerr << "Invalid value(s) assigned\n";
            return 1;
        }
        catch (const std::out_of_range& exc) {
            if (argc > 1)
                std::cerr << exc.what() << "\n\n";
            std::cerr << "Usage: " << std::string(argv[0]);
            std::cerr << " <OPTION...>\n\n";
            std::cerr << "-s,  --size\t\tsize of the process queue\n";
            std::cerr << "-q,  --quantum\t\ttime quantum (ms) value for round robin scheduling\n";
            std::cerr << "-rn, --rate-arrived\tpriority increment rate (/ms) for newly arrived processes\n";
            std::cerr << "-ra, --rate-accepted\tpriority increment rate (/ms) for accepted processes\n";
            return 1;
        }

        ProcessQueue waiting(size);
        populate_queue(waiting);

        SelfishRoundRobin srr(std::ref(waiting), quantum);
        std::thread selfish_rr = srr.start(rn, ra);
        selfish_rr.join();
        return 0;
    }
    catch (const std::exception& exc) {
        std::cerr << "Exception(s) caught:\n\n";
        std::cerr << exc.what() << std::endl;
        return 1;
    }
}
