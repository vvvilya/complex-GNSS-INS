#include <iostream>
#include <fstream>
#include <vector>
#include <thread>
#include <mutex>
#include <chrono>
#include <cmath>
#include <random>
#include <iomanip>
#include <Eigen/Dense>
#include <atomic>
#include <sstream>
#include <optional>
#include <ctime>
#include <algorithm> 

// Константы
constexpr double EARTH_RADIUS = 6378137.0; // метры
constexpr double G = 9.80665; // м/с^2
constexpr double PI = 3.14159265358979323846;
constexpr int STATE_SIZE = 9;  // position(3) + velocity(3) + acceleration(3)
constexpr int MEASUREMENT_SIZE = 6;  // position(3) + velocity(3)

// Класс для логирования
class Logger {
private:
    static std::ofstream log_file;
    static std::mutex log_mutex;
    static const char* log_filename;
    static bool initialized;

public:
    enum Level {
        DEBUG,
        INFO,
        ERROR
    };

    static bool init() {
        std::lock_guard<std::mutex> lock(log_mutex);
        try {
            if (!initialized) {
                log_file.open(log_filename, std::ios::out | std::ios::trunc);
                if (log_file.is_open()) {
                    initialized = true;
                    std::time_t now = std::time(nullptr);
                    log_file << "=== Log started at " 
                            << std::put_time(std::localtime(&now), "%Y-%m-%d %H:%M:%S")
                            << " ===" << std::endl;
                    return true;
                }
            }
            return initialized;
        }
        catch (const std::exception& e) {
            std::cerr << "Failed to initialize logger: " << e.what() << std::endl;
            return false;
        }
    }

    static void log(Level level, const std::string& message) {
        if (!initialized) return;

        std::lock_guard<std::mutex> lock(log_mutex);
        auto now = std::chrono::system_clock::now();
        std::time_t now_c = std::chrono::system_clock::to_time_t(now);
        
        log_file << std::put_time(std::localtime(&now_c), "%Y-%m-%d %H:%M:%S") 
                << " [" << getLevelString(level) << "] " 
                << message << std::endl;
        log_file.flush();

        // Дублируем важные сообщения в консоль
        if (level == ERROR || level == INFO) {
            std::cout << "[" << getLevelString(level) << "] " << message << std::endl;
        }
    }

private:
    static const char* getLevelString(Level level) {
        switch (level) {
            case DEBUG: return "DEBUG";
            case INFO:  return "INFO";
            case ERROR: return "ERROR";
            default:    return "UNKNOWN";
        }
    }
};

std::ofstream Logger::log_file;
std::mutex Logger::log_mutex;
const char* Logger::log_filename = "navigation.log";
bool Logger::initialized = false;

// Базовые структуры данных
struct Position {
    double latitude;   // широта
    double longitude;  // долгота
    double altitude;   // высота
    
    Position(double lat = 55.75583, double lon = 37.61778, double alt = 200.0)
        : latitude(lat), longitude(lon), altitude(alt) {}

    std::string toString() const {
        std::stringstream ss;
        ss << std::fixed << std::setprecision(6)
           << "(" << latitude << ", " << longitude << ", " 
           << std::setprecision(2) << altitude << ")";
        return ss.str();
    }
};

struct Waypoint {
    Position position;
    bool reached;
    std::chrono::system_clock::time_point reached_time;

    Waypoint(const Position& pos) 
        : position(pos), reached(false) {}
};

struct Attitude {
    double roll;
    double pitch;
    double yaw;
    
    Attitude(double r = 0.0, double p = 0.0, double y = 0.0)
        : roll(r), pitch(p), yaw(y) {}

    std::string toString() const {
        std::stringstream ss;
        ss << std::fixed << std::setprecision(2)
           << "(" << roll << "°, " << pitch << "°, " << yaw << "°)";
        return ss.str();
    }
};

struct IMUData {
    Eigen::Vector3d acceleration;
    Eigen::Vector3d angular_velocity;
    std::chrono::system_clock::time_point timestamp;

    IMUData() : acceleration(Eigen::Vector3d::Zero()),
                angular_velocity(Eigen::Vector3d::Zero()),
                timestamp(std::chrono::system_clock::now()) {}

    std::string toString() const {
        std::stringstream ss;
        ss << std::fixed << std::setprecision(3)
           << "Acc(" << acceleration.x() << ", " << acceleration.y() << ", " << acceleration.z() << ") "
           << "Gyro(" << angular_velocity.x() << ", " << angular_velocity.y() << ", " << angular_velocity.z() << ")";
        return ss.str();
    }
};

struct GNSSData {
    Position position;
    Eigen::Vector3d velocity;
    double hdop;
    int satellites_visible;
    std::chrono::system_clock::time_point timestamp;

    GNSSData() 
        : position()
        , velocity(Eigen::Vector3d::Zero())
        , hdop(1.0)
        , satellites_visible(0)
        , timestamp(std::chrono::system_clock::now()) {}

    std::string toString() const {
        std::stringstream ss;
        ss << "Pos" << position.toString() 
           << " Vel(" << velocity.x() << ", " << velocity.y() << ", " << velocity.z() << ")"
           << " HDOP:" << hdop << " Sats:" << satellites_visible;
        return ss.str();
    }
};

struct NavigationState {
    Position position;
    double deviation;
    bool is_running;
    bool gnss_available;
    double hdop;
    int satellites_visible;
    int current_waypoint;
    double distance_to_next;
    
    NavigationState(
        const Position& pos = Position(),
        double dev = 0.0,
        bool running = false,
        bool gnss = false,
        double hdop_val = 0.0,
        int sats = 0,
        int waypoint = 0,
        double dist = 0.0
    ) : position(pos),
        deviation(dev),
        is_running(running),
        gnss_available(gnss),
        hdop(hdop_val),
        satellites_visible(sats),
        current_waypoint(waypoint),
        distance_to_next(dist) {}

    std::string toString() const {
        std::stringstream ss;
        ss << "Position: " << position.toString() 
           << "\nDeviation: " << std::fixed << std::setprecision(2) << deviation << "m"
           << "\nGNSS: " << (gnss_available ? "Available" : "Unavailable")
           << " (HDOP:" << hdop << " Sats:" << satellites_visible << ")"
           << "\nWaypoint: " << current_waypoint 
           << " Distance: " << distance_to_next << "m";
        return ss.str();
    }
};

// Класс для вычисления маршрутных данных
class RouteManager {
private:
    std::vector<Waypoint> waypoints_;
    size_t current_waypoint_index_;
    const double waypoint_reach_threshold_{10.0}; // метров
    mutable std::mutex route_mutex_;

public:
    RouteManager() : current_waypoint_index_(0) {}

    static double calculateDistance(const Position& p1, const Position& p2) {
        double dlat = (p2.latitude - p1.latitude) * PI / 180.0;
        double dlon = (p2.longitude - p1.longitude) * PI / 180.0;
        
        double a = std::sin(dlat/2) * std::sin(dlat/2) +
                  std::cos(p1.latitude * PI / 180.0) * std::cos(p2.latitude * PI / 180.0) *
                  std::sin(dlon/2) * std::sin(dlon/2);
        
        double c = 2 * std::atan2(std::sqrt(a), std::sqrt(1-a));
        double ground_distance = EARTH_RADIUS * c;
        
        double height_diff = p2.altitude - p1.altitude;
        return std::sqrt(std::pow(ground_distance, 2) + std::pow(height_diff, 2));
    }

    // Вычисление курса между двумя точками
    static double calculateBearing(const Position& p1, const Position& p2) {
        double dLon = (p2.longitude - p1.longitude) * PI / 180.0;
        double lat1 = p1.latitude * PI / 180.0;
        double lat2 = p2.latitude * PI / 180.0;
        
        double y = std::sin(dLon) * std::cos(lat2);
        double x = std::cos(lat1) * std::sin(lat2) -
                  std::sin(lat1) * std::cos(lat2) * std::cos(dLon);
        
        double bearing = std::atan2(y, x);
        return std::fmod((bearing * 180.0 / PI + 360.0), 360.0);
    }

    bool loadRouteFromFile(const std::string& filename) {
        std::lock_guard<std::mutex> lock(route_mutex_);
        std::ifstream file(filename);
        if (!file.is_open()) {
            Logger::log(Logger::ERROR, "Failed to open route file: " + filename);
            return false;
        }
        
        waypoints_.clear();
        current_waypoint_index_ = 0;
        
        std::string line;
        while (std::getline(file, line)) {
            // Пропускаем комментарии и пустые строки
            if (line.empty() || line[0] == '#') continue;
            
            std::stringstream ss(line);
            double lat, lon, alt;
            if (ss >> lat >> lon >> alt) {
                waypoints_.emplace_back(Position(lat, lon, alt));
                Logger::log(Logger::DEBUG, "Loaded waypoint: " + waypoints_.back().position.toString());
            }
        }

        if (waypoints_.empty()) {
            Logger::log(Logger::ERROR, "No valid waypoints found in route file");
            return false;
        }

        Logger::log(Logger::INFO, "Loaded " + std::to_string(waypoints_.size()) + " waypoints");
        return true;
    }

    double calculateRouteDeviation(const Position& current_position) const {
        std::lock_guard<std::mutex> lock(route_mutex_);
        
        if (waypoints_.empty() || current_waypoint_index_ >= waypoints_.size()) {
            return std::numeric_limits<double>::max();
        }

        // Вычисляем отклонение от линии между текущей и следующей точкой маршрута
        const Position& current_waypoint = waypoints_[current_waypoint_index_].position;
        
        // Если это последняя точка, считаем отклонение до нее
        if (current_waypoint_index_ == waypoints_.size() - 1) {
            return calculateDistance(current_position, current_waypoint);
        }

        const Position& next_waypoint = waypoints_[current_waypoint_index_ + 1].position;
        
        // Вычисляем отклонение от линии маршрута
        double a = calculateDistance(current_position, current_waypoint);
        double b = calculateDistance(current_position, next_waypoint);
        double c = calculateDistance(current_waypoint, next_waypoint);
        
        double p = (a + b + c) / 2.0;
        double area = std::sqrt(p * (p - a) * (p - b) * (p - c));
        double deviation = (2.0 * area) / c;

        return deviation;
    }

    void updateWaypointProgress(const Position& current_position) {
        std::lock_guard<std::mutex> lock(route_mutex_);
        
        if (waypoints_.empty() || current_waypoint_index_ >= waypoints_.size()) {
            return;
        }

        double distance = calculateDistance(current_position, 
                                         waypoints_[current_waypoint_index_].position);

        if (distance < waypoint_reach_threshold_) {
            waypoints_[current_waypoint_index_].reached = true;
            waypoints_[current_waypoint_index_].reached_time = std::chrono::system_clock::now();
            
            Logger::log(Logger::INFO, "Reached waypoint " + 
                       std::to_string(current_waypoint_index_) + ": " +
                       waypoints_[current_waypoint_index_].position.toString());
            
            if (current_waypoint_index_ < waypoints_.size() - 1) {
                current_waypoint_index_++;
                Logger::log(Logger::INFO, "Moving to next waypoint: " + 
                           waypoints_[current_waypoint_index_].position.toString());
            }
        }
    }

    size_t getCurrentWaypointIndex() const {
        std::lock_guard<std::mutex> lock(route_mutex_);
        return current_waypoint_index_;
    }

    double getDistanceToNextWaypoint(const Position& current_position) const {
        std::lock_guard<std::mutex> lock(route_mutex_);
        if (waypoints_.empty() || current_waypoint_index_ >= waypoints_.size()) {
            return std::numeric_limits<double>::max();
        }
        return calculateDistance(current_position, waypoints_[current_waypoint_index_].position);
    }

    bool isRouteComplete() const {
        std::lock_guard<std::mutex> lock(route_mutex_);
        return !waypoints_.empty() && 
               current_waypoint_index_ == waypoints_.size() - 1 &&
               waypoints_.back().reached;
    }

    // Получение точки следования
    Position getTargetPosition() const {
        std::lock_guard<std::mutex> lock(route_mutex_);
        if (waypoints_.empty() || current_waypoint_index_ >= waypoints_.size()) {
            return Position();
        }
        return waypoints_[current_waypoint_index_].position;
    }

    // Получение следующей точки маршрута
    std::optional<Position> getNextWaypoint() const {
        std::lock_guard<std::mutex> lock(route_mutex_);
        if (current_waypoint_index_ + 1 < waypoints_.size()) {
            return waypoints_[current_waypoint_index_ + 1].position;
        }
        return std::nullopt;
    }
};

// Класс для моделирования движения БПЛА
class UAVModel {
private:
    Position current_position_;
    Attitude current_attitude_;
    Eigen::Vector3d velocity_;
    Eigen::Vector3d acceleration_;
    double target_speed_;  // м/с
    double max_acceleration_;  // м/с²
    mutable std::mutex state_mutex_;

public:
    UAVModel() 
        : velocity_(Eigen::Vector3d::Zero())
        , acceleration_(Eigen::Vector3d::Zero())
        , target_speed_(15.0)  // 15 м/с ≈ 54 км/ч
        , max_acceleration_(2.0)  // 2 м/с²
    {
        Logger::log(Logger::INFO, "UAV model initialized");
    }

    void updateState(double dt, const Position& target_position) {
        std::lock_guard<std::mutex> lock(state_mutex_);

        try {
            // Вычисляем направление к цели
            double bearing = RouteManager::calculateBearing(current_position_, target_position);
            double distance = RouteManager::calculateDistance(current_position_, target_position);

            // Преобразуем курс в радианы
            double bearing_rad = bearing * PI / 180.0;

            // Вычисляем желаемую скорость
            double desired_speed = std::min(target_speed_, distance);
            
            // Вычисляем желаемые компоненты скорости
            Eigen::Vector3d desired_velocity;
            desired_velocity.x() = desired_speed * std::cos(bearing_rad);
            desired_velocity.y() = desired_speed * std::sin(bearing_rad);
            desired_velocity.z() = (target_position.altitude - current_position_.altitude) / dt;

            // Ограничиваем ускорение
            Eigen::Vector3d velocity_diff = desired_velocity - velocity_;
            if (velocity_diff.norm() > max_acceleration_ * dt) {
                velocity_diff = velocity_diff.normalized() * max_acceleration_ * dt;
            }

            // Обновляем скорость и положение
            velocity_ += velocity_diff;
            
            // Обновляем позицию
            double dlat = velocity_.x() * dt / EARTH_RADIUS * (180.0 / PI);
            double dlon = velocity_.y() * dt / (EARTH_RADIUS * 
                         cos(current_position_.latitude * PI / 180.0)) * (180.0 / PI);
            
            current_position_.latitude += dlat;
            current_position_.longitude += dlon;
            current_position_.altitude += velocity_.z() * dt;

            // Логируем обновление состояния
            std::stringstream ss;
            ss << "UAV state updated:"
               << "\nPosition: " << current_position_.toString()
               << "\nVelocity: " << velocity_.norm() << " m/s"
               << "\nBearing: " << bearing << "°"
               << "\nDistance to target: " << distance << "m";
            Logger::log(Logger::DEBUG, ss.str());
        }
        catch (const std::exception& e) {
            Logger::log(Logger::ERROR, "Error updating UAV state: " + std::string(e.what()));
            throw;
        }
    }

    Position getPosition() const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return current_position_;
    }

    void setPosition(const Position& pos) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        current_position_ = pos;
        Logger::log(Logger::INFO, "UAV position set to: " + pos.toString());
    }

    Eigen::Vector3d getVelocity() const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return velocity_;
    }

    void setTargetSpeed(double speed) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        target_speed_ = speed;
        Logger::log(Logger::INFO, "Target speed set to: " + std::to_string(speed) + " m/s");
    }
};

// Класс для моделирования инерциальной навигационной системы
class INS {
private:
    std::random_device rd_;
    std::mt19937 gen_;
    std::normal_distribution<> noise_dist_;
    
    double accelerometer_bias_;
    double gyro_bias_;
    
    IMUData current_imu_data_;
    Position estimated_position_;
    Eigen::Vector3d estimated_velocity_;
    mutable std::mutex ins_mutex_;

public:
    INS() 
        : gen_(rd_())
        , noise_dist_(0.0, 0.01)
        , accelerometer_bias_(0.0001)
        , gyro_bias_(0.0001)
        , estimated_velocity_(Eigen::Vector3d::Zero())
    {
        Logger::log(Logger::INFO, "INS initialized");
    }

    IMUData generateIMUData(const Position& true_position, const Eigen::Vector3d& true_velocity) {
        std::lock_guard<std::mutex> lock(ins_mutex_);
        
        IMUData data;
        
        // Добавляем реалистичные ошибки и шумы
        for(int i = 0; i < 3; ++i) {
            // Ускорение с учетом гравитации для вертикальной составляющей
            double base_acceleration = (i == 2) ? -G : true_velocity[i];
            data.acceleration(i) = base_acceleration + noise_dist_(gen_) + accelerometer_bias_;
            data.angular_velocity(i) = noise_dist_(gen_) + gyro_bias_;
        }
        
        data.timestamp = std::chrono::system_clock::now();

        Logger::log(Logger::DEBUG, "Generated IMU data: " + data.toString());
        return data;
    }

    void updatePosition(const IMUData& imu_data, double dt) {
        std::lock_guard<std::mutex> lock(ins_mutex_);
        
        try {
            Position prev_position = estimated_position_;
            Eigen::Vector3d prev_velocity = estimated_velocity_;

            // Компенсация смещения и интегрирование
            Eigen::Vector3d corrected_acceleration = imu_data.acceleration;
            corrected_acceleration(2) += G; // Компенсация гравитации

            // Интегрирование ускорения для получения скорости
            estimated_velocity_ += corrected_acceleration * dt;
            
            // Обновление позиции
            double dlat = estimated_velocity_.x() * dt / EARTH_RADIUS * (180.0 / PI);
            double dlon = estimated_velocity_.y() * dt / (EARTH_RADIUS * 
                         cos(estimated_position_.latitude * PI / 180.0)) * (180.0 / PI);
            
            estimated_position_.latitude += dlat;
            estimated_position_.longitude += dlon;
            estimated_position_.altitude += estimated_velocity_.z() * dt;

            std::stringstream ss;
            ss << "INS update:"
               << "\nPrevious position: " << prev_position.toString()
               << "\nNew position: " << estimated_position_.toString()
               << "\nVelocity change: " << (estimated_velocity_ - prev_velocity).norm() << " m/s";
            Logger::log(Logger::DEBUG, ss.str());
        }
        catch (const std::exception& e) {
            Logger::log(Logger::ERROR, "Error in INS update: " + std::string(e.what()));
            throw;
        }
    }

    Position getEstimatedPosition() const {
        std::lock_guard<std::mutex> lock(ins_mutex_);
        return estimated_position_;
    }

    void setInitialPosition(const Position& pos) {
        std::lock_guard<std::mutex> lock(ins_mutex_);
        estimated_position_ = pos;
        estimated_velocity_ = Eigen::Vector3d::Zero();
        Logger::log(Logger::INFO, "INS initial position set to: " + pos.toString());
    }
};

// Класс для моделирования ГНСС
class GNSS {
private:
    std::random_device rd_;
    std::mt19937 gen_;
    std::normal_distribution<> noise_dist_;
    double position_error_stddev_;
    mutable std::mutex gnss_mutex_;
    
    // Параметры моделирования помех
    struct SignalQuality {
        double base_hdop;
        int base_satellites;
        double interference_level;
        
        SignalQuality() 
            : base_hdop(1.0)
            , base_satellites(12)
            , interference_level(0.0) {}
    } signal_quality_;

public:
    GNSS() 
        : gen_(rd_())
        , noise_dist_(0.0, 5.0)
        , position_error_stddev_(5.0)
    {
        Logger::log(Logger::INFO, "GNSS initialized");
    }

    GNSSData generateGNSSData(const Position& true_position, const Eigen::Vector3d& true_velocity) {
        std::lock_guard<std::mutex> lock(gnss_mutex_);
        
        try {
            GNSSData data;
            
            // Моделирование ошибок позиционирования
            double position_error = position_error_stddev_ * (1.0 + signal_quality_.interference_level);
            data.position.latitude = true_position.latitude + 
                noise_dist_(gen_) * position_error / EARTH_RADIUS * (180.0 / PI);
            data.position.longitude = true_position.longitude + 
                noise_dist_(gen_) * position_error / (EARTH_RADIUS * 
                cos(true_position.latitude * PI / 180.0)) * (180.0 / PI);
            data.position.altitude = true_position.altitude + noise_dist_(gen_) * position_error;

            // Моделирование ошибок скорости
            for(int i = 0; i < 3; ++i) {
                data.velocity(i) = true_velocity(i) + noise_dist_(gen_) * 0.1;
            }

            // Моделирование качества сигнала
            data.hdop = signal_quality_.base_hdop * (1.0 + std::abs(noise_dist_(gen_)) * 
                       signal_quality_.interference_level);
            data.satellites_visible = static_cast<int>(signal_quality_.base_satellites * 
                (1.0 - signal_quality_.interference_level));
            
            data.timestamp = std::chrono::system_clock::now();

            std::stringstream ss;
            ss << "Generated GNSS data: " << data.toString() 
               << "\nSignal quality - HDOP: " << data.hdop 
               << ", Satellites: " << data.satellites_visible;
            Logger::log(Logger::DEBUG, ss.str());

            return data;
        }
        catch (const std::exception& e) {
            Logger::log(Logger::ERROR, "Error generating GNSS data: " + std::string(e.what()));
            throw;
        }
    }

    void updateSignalQuality(double interference) {
        std::lock_guard<std::mutex> lock(gnss_mutex_);
        signal_quality_.interference_level = std::clamp(interference, 0.0, 1.0);
        Logger::log(Logger::INFO, "GNSS signal quality updated, interference level: " + 
                   std::to_string(interference));
    }
};

// Класс для реализации фильтра Калмана
class KalmanFilter {
private:
    Eigen::VectorXd state_;            // Вектор состояния [position, velocity, acceleration]
    Eigen::MatrixXd covariance_;       // Матрица ковариации
    Eigen::MatrixXd process_noise_;    // Шум процесса
    Eigen::MatrixXd measurement_noise_; // Шум измерений
    Eigen::MatrixXd transition_matrix_; // Матрица перехода состояния
    bool is_initialized_;

public:
    KalmanFilter() : is_initialized_(false) {
        // Инициализация матриц
        state_ = Eigen::VectorXd::Zero(STATE_SIZE);
        covariance_ = Eigen::MatrixXd::Identity(STATE_SIZE, STATE_SIZE) * 100.0;
        process_noise_ = Eigen::MatrixXd::Identity(STATE_SIZE, STATE_SIZE) * 0.01;
        measurement_noise_ = Eigen::MatrixXd::Identity(3, 3) * 0.1;
        transition_matrix_ = Eigen::MatrixXd::Identity(STATE_SIZE, STATE_SIZE);

        Logger::log(Logger::INFO, "Kalman filter initialized");
    }

    void setState(const Position& pos, const Eigen::Vector3d& vel) {
        state_.segment<3>(0) << pos.latitude, pos.longitude, pos.altitude;
        state_.segment<3>(3) = vel;
        state_.segment<3>(6).setZero();
        is_initialized_ = true;

        Logger::log(Logger::INFO, "Kalman filter state set to position: " + pos.toString());
    }

    void predict(double dt) {
        try {
            // Обновление матрицы перехода состояния
            transition_matrix_.block<3,3>(0,3) = Eigen::Matrix3d::Identity() * dt;
            transition_matrix_.block<3,3>(3,6) = Eigen::Matrix3d::Identity() * dt;
            
            // Прогноз состояния
            state_ = transition_matrix_ * state_;
            
            // Обновление ковариации
            covariance_ = transition_matrix_ * covariance_ * transition_matrix_.transpose() + 
                         process_noise_ * dt;

            std::stringstream ss;
            ss << "KF prediction:"
               << "\nPosition: (" << state_[0] << ", " << state_[1] << ", " << state_[2] << ")"
               << "\nVelocity: (" << state_[3] << ", " << state_[4] << ", " << state_[5] << ")";
            Logger::log(Logger::DEBUG, ss.str());
        }
        catch (const std::exception& e) {
            Logger::log(Logger::ERROR, "Error in Kalman prediction: " + std::string(e.what()));
            throw;
        }
    }

    void update(const Eigen::VectorXd& measurement, const Eigen::MatrixXd& H) {
        try {
            if (!is_initialized_) {
                throw std::runtime_error("Kalman filter not initialized");
            }

            // Подготовка матрицы шума измерений соответствующего размера
            Eigen::MatrixXd R = Eigen::MatrixXd::Identity(measurement.size(), measurement.size()) * 0.1;

            // Вычисление инновации
            Eigen::VectorXd innovation = measurement - H * state_;
            
            // Вычисление матрицы усиления Калмана
            Eigen::MatrixXd S = H * covariance_ * H.transpose() + R;
            Eigen::MatrixXd K = covariance_ * H.transpose() * S.inverse();
            
            // Обновление состояния и ковариации
            state_ = state_ + K * innovation;
            covariance_ = (Eigen::MatrixXd::Identity(STATE_SIZE, STATE_SIZE) - K * H) * covariance_;

            std::stringstream ss;
            ss << "KF update completed:"
               << "\nInnovation norm: " << innovation.norm()
               << "\nUpdated position: (" << state_[0] << ", " << state_[1] << ", " << state_[2] << ")";
            Logger::log(Logger::DEBUG, ss.str());
        }
        catch (const std::exception& e) {
            Logger::log(Logger::ERROR, "Error in Kalman update: " + std::string(e.what()));
            throw;
        }
    }

    Position getPosition() const {
        return Position(state_[0], state_[1], state_[2]);
    }

    Eigen::Vector3d getVelocity() const {
        return state_.segment<3>(3);
    }
};

// Класс гибридной навигационной системы
class HybridNavigationSystem {
private:
    UAVModel uav_;
    INS ins_;
    GNSS gnss_;
    KalmanFilter kf_;
    RouteManager route_manager_;
    
    bool use_tight_coupling_;
    mutable std::mutex integration_mutex_;
    GNSSData last_gnss_data_;
    
    double ins_error_;
    double gnss_error_;
    std::chrono::system_clock::time_point last_gnss_update_;
    const std::chrono::seconds gnss_timeout_{3}; // Таймаут ГНСС
    
public:
    HybridNavigationSystem() 
        : use_tight_coupling_(false)
        , ins_error_(0.0)
        , gnss_error_(0.0)
        , last_gnss_update_(std::chrono::system_clock::now())
    {
        Logger::log(Logger::INFO, "Hybrid navigation system initialized");
    }
    
    bool initialize(const std::string& route_file) {
        try {
            if (!route_manager_.loadRouteFromFile(route_file)) {
                return false;
            }

            // Установка начальной позиции
            Position initial_position = route_manager_.getTargetPosition();
            uav_.setPosition(initial_position);
            ins_.setInitialPosition(initial_position);
            kf_.setState(initial_position, Eigen::Vector3d::Zero());

            Logger::log(Logger::INFO, "Navigation system initialized with position: " + 
                       initial_position.toString());
            return true;
        }
        catch (const std::exception& e) {
            Logger::log(Logger::ERROR, "Failed to initialize navigation system: " + 
                       std::string(e.what()));
            return false;
        }
    }
    
    void processINSData(const IMUData& imu_data, double dt) {
        std::lock_guard<std::mutex> lock(integration_mutex_);
        
        try {
            // Обновление ИНС
            ins_.updatePosition(imu_data, dt);
            
            // Прогноз в фильтре Калмана
            kf_.predict(dt);

            // Обновление позиции БПЛА
            Position target = route_manager_.getTargetPosition();
            uav_.updateState(dt, target);

            // Проверка достижения точек маршрута
            route_manager_.updateWaypointProgress(uav_.getPosition());

            Logger::log(Logger::DEBUG, "Processed INS data with dt: " + std::to_string(dt));
        }
        catch (const std::exception& e) {
            Logger::log(Logger::ERROR, "Error processing INS data: " + std::string(e.what()));
            throw;
        }
    }
    
    void processGNSSData(const GNSSData& gnss_data) {
        std::lock_guard<std::mutex> lock(integration_mutex_);
        
        try {
            last_gnss_data_ = gnss_data;
            last_gnss_update_ = std::chrono::system_clock::now();

            // Подготовка измерений для фильтра Калмана
            Eigen::VectorXd measurement(3);
            measurement << gnss_data.position.latitude,
                          gnss_data.position.longitude,
                          gnss_data.position.altitude;
            
            // Матрица измерений (связывает состояние с измерениями)
            Eigen::MatrixXd H = Eigen::MatrixXd::Zero(3, STATE_SIZE);
            H.block<3,3>(0,0) = Eigen::Matrix3d::Identity();
            
            // Обновление фильтра Калмана
            kf_.update(measurement, H);

            // Оценка качества сигнала и выбор режима интеграции
            evaluateGNSSQuality(gnss_data);

            Logger::log(Logger::DEBUG, "Processed GNSS data: " + gnss_data.toString());
        }
        catch (const std::exception& e) {
            Logger::log(Logger::ERROR, "Error processing GNSS data: " + std::string(e.what()));
            throw;
        }
    }
    
    void evaluateGNSSQuality(const GNSSData& gnss_data) {
        double quality_score = 1.0 / gnss_data.hdop * gnss_data.satellites_visible / 12.0;
        
        if (quality_score < 0.5 && use_tight_coupling_) {
            use_tight_coupling_ = false;
            Logger::log(Logger::INFO, "Switched to loose coupling due to poor GNSS quality");
        } 
        else if (quality_score > 0.8 && !use_tight_coupling_) {
            use_tight_coupling_ = true;
            Logger::log(Logger::INFO, "Switched to tight coupling due to good GNSS quality");
        }
    }
    
    NavigationState getState() const {
        std::lock_guard<std::mutex> lock(integration_mutex_);
        
        Position current_position = uav_.getPosition();
        double deviation = route_manager_.calculateRouteDeviation(current_position);
        
        // Проверка актуальности ГНСС данных
        bool gnss_available = (std::chrono::system_clock::now() - last_gnss_update_) < gnss_timeout_;
        
        return NavigationState{
            current_position,
            deviation,
            true,  // is_running
            gnss_available,
            last_gnss_data_.hdop,
            last_gnss_data_.satellites_visible,
            static_cast<int>(route_manager_.getCurrentWaypointIndex()),
            route_manager_.getDistanceToNextWaypoint(current_position)
        };
    }

    bool isRouteComplete() const {
        return route_manager_.isRouteComplete();
    }

    Position getCurrentPosition() const {
        std::lock_guard<std::mutex> lock(integration_mutex_);
        return uav_.getPosition();
    }

    GNSSData getLastGNSSData() const {
        std::lock_guard<std::mutex> lock(integration_mutex_);
        return last_gnss_data_;
    }
};

// Класс симуляции навигации
class NavigationSimulation {
private:
    HybridNavigationSystem nav_system_;
    std::atomic<bool> running_;
    
    std::thread ins_thread_;
    std::thread gnss_thread_;
    std::thread deviation_thread_;
    
    const std::chrono::milliseconds INS_UPDATE_INTERVAL{10};    // 100 Hz
    const std::chrono::milliseconds GNSS_UPDATE_INTERVAL{1000}; // 1 Hz
    const std::chrono::milliseconds DEVIATION_CHECK_INTERVAL{100}; // 10 Hz
    
    void insLoop() {
        Logger::log(Logger::INFO, "Starting INS loop");
        
        while (running_) {
            auto start_time = std::chrono::steady_clock::now();
            
            try {
                Position current_pos = nav_system_.getCurrentPosition();
                Eigen::Vector3d velocity = Eigen::Vector3d::Zero(); // Заменить на реальную скорость

                IMUData imu_data = generateIMUData(current_pos, velocity);
                nav_system_.processINSData(imu_data, INS_UPDATE_INTERVAL.count() / 1000.0);
            }
            catch (const std::exception& e) {
                Logger::log(Logger::ERROR, std::string("Error in INS loop: ") + e.what());
            }
            
            std::this_thread::sleep_until(start_time + INS_UPDATE_INTERVAL);
        }
    }
    
    void gnssLoop() {
        Logger::log(Logger::INFO, "Starting GNSS loop");
        
        while (running_) {
            auto start_time = std::chrono::steady_clock::now();
            
            try {
                Position current_pos = nav_system_.getCurrentPosition();
                Eigen::Vector3d velocity = Eigen::Vector3d::Zero(); // Заменить на реальную скорость

                GNSSData gnss_data = generateGNSSData(current_pos, velocity);
                nav_system_.processGNSSData(gnss_data);
            }
            catch (const std::exception& e) {
                Logger::log(Logger::ERROR, std::string("Error in GNSS loop: ") + e.what());
            }
            
            std::this_thread::sleep_until(start_time + GNSS_UPDATE_INTERVAL);
        }
    }
    
    void deviationLoop() {
        Logger::log(Logger::INFO, "Starting deviation monitoring loop");
        
        while (running_) {
            auto start_time = std::chrono::steady_clock::now();
            
            try {
                NavigationState state = nav_system_.getState();
                
                // Очистка консоли и вывод текущего состояния
                std::cout << "\033[2J\033[H";  // Очистка экрана
                std::cout << "=== Navigation Status ===\n" << state.toString() << std::endl;
                
                if (nav_system_.isRouteComplete()) {
                    Logger::log(Logger::INFO, "Route completed!");
                    running_ = false;
                }
            }
            catch (const std::exception& e) {
                Logger::log(Logger::ERROR, std::string("Error in deviation loop: ") + e.what());
            }
            
            std::this_thread::sleep_until(start_time + DEVIATION_CHECK_INTERVAL);
        }
    }

    IMUData generateIMUData(const Position& current_pos, const Eigen::Vector3d& velocity) {
        static INS ins;
        return ins.generateIMUData(current_pos, velocity);
    }

    GNSSData generateGNSSData(const Position& current_pos, const Eigen::Vector3d& velocity) {
        static GNSS gnss;
        return gnss.generateGNSSData(current_pos, velocity);
    }
    
public:
    NavigationSimulation() : running_(false) {
        Logger::log(Logger::INFO, "Navigation simulation created");
    }
    
    ~NavigationSimulation() {
        if (running_) {
            stop();
        }
    }
    
    bool initialize(const std::string& route_file) {
        try {
            if (!nav_system_.initialize(route_file)) {
                Logger::log(Logger::ERROR, "Failed to initialize navigation system");
                return false;
            }
            Logger::log(Logger::INFO, "Navigation simulation initialized successfully");
            return true;
        }
        catch (const std::exception& e) {
            Logger::log(Logger::ERROR, std::string("Error during initialization: ") + e.what());
            return false;
        }
    }
    
    void start() {
        Logger::log(Logger::INFO, "Starting navigation simulation...");
        
        try {
            running_ = true;
            ins_thread_ = std::thread(&NavigationSimulation::insLoop, this);
            gnss_thread_ = std::thread(&NavigationSimulation::gnssLoop, this);
            deviation_thread_ = std::thread(&NavigationSimulation::deviationLoop, this);
        }
        catch (const std::exception& e) {
            Logger::log(Logger::ERROR, std::string("Failed to start simulation threads: ") + e.what());
            stop();
            throw;
        }
    }
    
    void stop() {
        Logger::log(Logger::INFO, "Stopping navigation simulation...");
        running_ = false;
        
        if (ins_thread_.joinable()) ins_thread_.join();
        if (gnss_thread_.joinable()) gnss_thread_.join();
        if (deviation_thread_.joinable()) deviation_thread_.join();
        
        Logger::log(Logger::INFO, "Simulation stopped");
    }
    
    NavigationState getState() const {
        return nav_system_.getState();
    }
};

int main(int argc, char** argv) {
    try {
        // Инициализация логгера
        if (!Logger::init()) {
            std::cerr << "Failed to initialize logger" << std::endl;
            return 1;
        }
        Logger::log(Logger::INFO, "=== Navigation System Started ===");

        // Проверка аргументов командной строки
        std::string route_file = "terrain.txt";
        if (argc > 1) {
            route_file = argv[1];
        }
        Logger::log(Logger::INFO, "Using route file: " + route_file);

        // Создание и инициализация симуляции
        NavigationSimulation simulation;
        
        if (!simulation.initialize(route_file)) {
            Logger::log(Logger::ERROR, "Failed to initialize simulation");
            std::cerr << "Failed to initialize simulation" << std::endl;
            return 1;
        }
        
        Logger::log(Logger::INFO, "Simulation initialized successfully");

        // Запуск симуляции
        std::cout << "\033[2J\033[H";  // Очистка экрана
        std::cout << "=== UAV Navigation Simulation ===\n"
                 << "Press 'q' to quit, 'p' to pause/resume\n"
                 << "Starting simulation..." << std::endl;
        
        simulation.start();

        // Основной цикл мониторинга
        bool paused = false;
        auto start_time = std::chrono::steady_clock::now();
        const auto simulation_duration = std::chrono::minutes(5);

        while (true) {
            try {
                // Проверка времени работы
                auto current_time = std::chrono::steady_clock::now();
                if (current_time - start_time >= simulation_duration) {
                    std::cout << "\nSimulation time limit reached." << std::endl;
                    Logger::log(Logger::INFO, "Simulation time limit reached");
                    break;
                }

                // Обработка пользовательского ввода
                if (std::cin.rdbuf()->in_avail()) {
                    char input;
                    std::cin.get(input);
                    
                    if (input == 'q' || input == 'Q') {
                        std::cout << "\nUser requested stop." << std::endl;
                        Logger::log(Logger::INFO, "User requested simulation stop");
                        break;
                    }
                    else if (input == 'p' || input == 'P') {
                        paused = !paused;
                        std::cout << "\nSimulation " << (paused ? "paused" : "resumed") << std::endl;
                        Logger::log(Logger::INFO, "Simulation " + 
                                  std::string(paused ? "paused" : "resumed"));
                        continue;
                    }
                }

                if (!paused) {
                    // Получение текущего состояния
                    NavigationState state = simulation.getState();

                    // Вывод информации о маршруте
                    std::cout << "\033[H";  // Возврат курсора в начало
                    std::cout << "=== Navigation Status ===\n" << state.toString() << std::endl;
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            catch (const std::exception& e) {
                Logger::log(Logger::ERROR, "Error in main loop: " + std::string(e.what()));
                std::cerr << "Error: " << e.what() << std::endl;
                break;
            }
        }

        // Корректное завершение работы
        std::cout << "\nStopping simulation..." << std::endl;
        simulation.stop();
        
        // Вывод итоговой статистики
        try {
            NavigationState final_state = simulation.getState();
            
            std::stringstream ss;
            ss << "\nFinal Navigation Statistics:"
               << "\nFinal position: " << final_state.position.toString()
               << "\nFinal deviation: " << std::fixed << std::setprecision(2) 
               << final_state.deviation << " meters"
               << "\nCompleted waypoints: " << final_state.current_waypoint
               << "\nGNSS quality: " << (final_state.gnss_available ? "Good" : "Poor")
               << " (Satellites: " << final_state.satellites_visible 
               << ", HDOP: " << final_state.hdop << ")";
            
            std::cout << ss.str() << std::endl;
            Logger::log(Logger::INFO, "Final statistics: " + ss.str());
        }
        catch (const std::exception& e) {
            Logger::log(Logger::ERROR, "Error getting final state: " + std::string(e.what()));
        }

        Logger::log(Logger::INFO, "=== Navigation System Finished ===");
        return 0;
    }
    catch (const std::exception& e) {
        Logger::log(Logger::ERROR, "Fatal error: " + std::string(e.what()));
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }
}