// main.cpp

#include <iostream>
#include <fstream>
#include <random>
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
#include "logger.hpp"      
#include "data_logger.hpp"

// Константы
constexpr double EARTH_RADIUS = 6378137.0; // метры
constexpr double G = 9.80665; // м/с^2
constexpr double PI = 3.14159265358979323846;
constexpr int STATE_SIZE = 9;  // position(3) + velocity(3) + acceleration(3)
constexpr int MEASUREMENT_SIZE = 6;  // position(3) + velocity(3)

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

struct Attitude {   //  позиция
    double roll;    //  крен
    double pitch;   //  тангаж
    double yaw;     //  рысканье
    
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
    double current_speed_kmh;
    
    NavigationState(
        const Position& pos = Position(),
        double dev = 0.0,
        bool running = false,
        bool gnss = false,
        double hdop_val = 0.0,
        int sats = 0,
        int waypoint = 0,
        double dist = 0.0,
        double speed = 0.0
    ) : position(pos),
        deviation(dev),
        is_running(running),
        gnss_available(gnss),
        hdop(hdop_val),
        satellites_visible(sats),
        current_waypoint(waypoint),
        distance_to_next(dist),
        current_speed_kmh(speed) {}

     std::string toString() const {
        std::stringstream ss;
        ss << "Position: " << position.toString() 
           << "\nDeviation: " << std::fixed << std::setprecision(2) << deviation << "m"
           << "\nGNSS: " << (gnss_available ? "Available" : "Unavailable")
           << " (HDOP:" << hdop << " Sats:" << satellites_visible << ")"
           << "\nWaypoint: " << current_waypoint 
           << "\nDistance to next: " << distance_to_next << "m"
           << "\nCurrent speed: " << std::fixed << std::setprecision(1) 
           << current_speed_kmh << " km/h";
        return ss.str();
    }
};

// Добавляем структуру для барометрических данных
struct BaroData {
    double altitude;      // высота в метрах
    double pressure;      // давление в гПа
    double temperature;   // температура в °C
    std::chrono::system_clock::time_point timestamp;

    BaroData() 
        : altitude(0.0)
        , pressure(1013.25)  // стандартное давление на уровне моря
        , temperature(15.0)   // стандартная температура
        , timestamp(std::chrono::system_clock::now()) {}

    std::string toString() const {
        std::stringstream ss;
        ss << std::fixed << std::setprecision(2)
           << "Alt: " << altitude << "m, "
           << "Press: " << pressure << "hPa, "
           << "Temp: " << temperature << "°C";
        return ss.str();
    }
};

// Класс барометрического высотомера
class Barometer {
private:
    std::random_device rd_;
    std::mt19937 gen_;
    std::normal_distribution<> noise_dist_;
    const double PRESSURE_NOISE = 0.1;    // шум давления в гПа
    const double TEMP_NOISE = 0.05;       // шум температуры в °C
    const double ALT_ERROR = 0.2;         // ошибка высоты в метрах

public:
    Barometer() 
        : gen_(rd_())
        , noise_dist_(0.0, 1.0)
    {}

    BaroData generateBaroData(double true_altitude) {
        BaroData data;
        
        // Вычисление давления по международной барометрической формуле
        // P = P0 * (1 - 0.0065 * h / T0)^5.255
        const double P0 = 1013.25;  // давление на уровне моря
        const double T0 = 288.15;   // температура на уровне моря в Кельвинах
        
        // Добавляем шум к истинной высоте
        double noisy_altitude = true_altitude + noise_dist_(gen_) * ALT_ERROR;
        
        // Вычисляем давление с учетом высоты
        double pressure = P0 * std::pow(1.0 - 0.0065 * noisy_altitude / T0, 5.255);
        pressure += noise_dist_(gen_) * PRESSURE_NOISE;
        
        // Температура уменьшается с высотой примерно на 6.5°C на км
        double temperature = 15.0 - (noisy_altitude / 1000.0) * 6.5;
        temperature += noise_dist_(gen_) * TEMP_NOISE;
        
        data.altitude = noisy_altitude;
        data.pressure = pressure;
        data.temperature = temperature;
        data.timestamp = std::chrono::system_clock::now();
        
        return data;
    }
};


// Класс для вычисления маршрутных данных
class RouteManager {
private:
    std::vector<Waypoint> waypoints_;
    size_t current_waypoint_index_;
    const double waypoint_reach_threshold_{10.0}; // порог достижения путевой точки (метры)
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

        const Position& current_waypoint = waypoints_[current_waypoint_index_].position;
        
        if (current_waypoint_index_ == waypoints_.size() - 1) {
            return calculateDistance(current_position, current_waypoint);
        }

        const Position& next_waypoint = waypoints_[current_waypoint_index_ + 1].position;
        
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

    Position getTargetPosition() const {
        std::lock_guard<std::mutex> lock(route_mutex_);
        if (waypoints_.empty() || current_waypoint_index_ >= waypoints_.size()) {
            return Position();
        }
        return waypoints_[current_waypoint_index_].position;
    }

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

            // Определяем желаемую скорость
            double desired_speed;
            if (distance < 10.0) { // Если близко к цели
                desired_speed = std::min(target_speed_ * (distance / 10.0), target_speed_);
            } else {
                desired_speed = target_speed_;
            }
            
            double lat_rad = current_position_.latitude * PI / 180.0;
            
            // Масштабные коэффициенты для преобразования градусов в метры
            double meters_per_degree_lat = 111132.92 - 559.82 * cos(2.0 * lat_rad) + 
                                         1.175 * cos(4.0 * lat_rad) - 0.0023 * cos(6.0 * lat_rad);
            double meters_per_degree_lon = 111412.84 * cos(lat_rad) - 
                                         93.5 * cos(3.0 * lat_rad) + 0.118 * cos(5.0 * lat_rad);

            // Вычисляем желаемые компоненты скорости
            Eigen::Vector3d desired_velocity;
            desired_velocity.x() = desired_speed * cos(bearing_rad);
            desired_velocity.y() = desired_speed * sin(bearing_rad);
            desired_velocity.z() = (target_position.altitude - current_position_.altitude) * 0.2;

            // Ограничиваем ускорение
            Eigen::Vector3d velocity_diff = desired_velocity - velocity_;
            if (velocity_diff.norm() > max_acceleration_ * dt) {
                velocity_diff = velocity_diff.normalized() * max_acceleration_ * dt;
            }

            // Обновляем скорость и положение
            velocity_ += velocity_diff;
        
            // Обновляем позицию
            double dlat = velocity_.x() * dt / meters_per_degree_lat;
            double dlon = velocity_.y() * dt / meters_per_degree_lon;
            double dalt = velocity_.z() * dt;
            
            current_position_.latitude += dlat;
            current_position_.longitude += dlon;
            current_position_.altitude += dalt;

            

            // Логируем обновление состояния
            std::stringstream ss;
            ss << std::fixed << std::setprecision(6)
               << "UAV state:"
               << "\nPosition: " << current_position_.toString()
               << "\nActual speed: " << velocity_.norm() << " m/s"
               << "\nDesired speed: " << desired_speed << " m/s"
               << "\nDistance to target: " << distance << " m"
               << "\nBearing: " << bearing << "°";
            Logger::log(Logger::DEBUG, ss.str());
        }
        catch (const std::exception& e) {
            Logger::log(Logger::ERROR, "Error updating UAV state: " + std::string(e.what()));
            throw;
        }
    }

    double getCurrentSpeedKmh() const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return velocity_.norm() * 3.6; // переводим м/с в км/ч
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
    Eigen::Quaterniond orientation_;
    Eigen::Vector3d position_;           
    Eigen::Vector3d corrected_position_; 
    Eigen::Vector3d velocity_;
    double baro_altitude_;
    // Добавляем поля для генерации случайных чисел
    std::random_device rd_;
    std::mt19937 gen_;

    mutable std::mutex state_mutex_; 
    
    // Теперь только простая линейная ошибка в метрах
    double uncorrected_error_;  

    std::chrono::steady_clock::time_point last_update_;

    static constexpr double ERROR_RATE = 0.01;  // строго 1 см в секунду
    static constexpr double UPDATE_RATE = 0.1;   // 10 Hz

public:
    INS() : 
        orientation_(Eigen::Quaterniond::Identity()),
        position_(Eigen::Vector3d::Zero()),
        corrected_position_(Eigen::Vector3d::Zero()),
        velocity_(Eigen::Vector3d::Zero()),
        baro_altitude_(0.0),
        uncorrected_error_(0.0),
        last_update_(std::chrono::steady_clock::now())
    {}

    void updateBaroAltitude(double altitude) {
        baro_altitude_ = altitude;
    }

    double getCurrentError() const {
        return uncorrected_error_;
    }

     Eigen::Vector3d getVelocity() const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return velocity_;
    }

    Position getEstimatedPosition() const {
        // Возвращаем позицию без добавления ошибки
        Position base = convertECEFToGeodetic(position_);
        base.altitude = baro_altitude_;
        return base;
    }

   Position getCorrectedPosition() const {
        Position base = convertECEFToGeodetic(corrected_position_);
        
        double meters_per_degree = 111319.9;
        double error_degrees = uncorrected_error_ / meters_per_degree;
        
        base.latitude += error_degrees;
        base.altitude = baro_altitude_;
        
        return base;
    }


   void setInitialPosition(const Position& pos) {
        position_ = convertToECEF(pos);
        corrected_position_ = position_;
        velocity_ = Eigen::Vector3d::Zero();
        orientation_ = Eigen::Quaterniond::Identity();
        baro_altitude_ = pos.altitude;
        
        uncorrected_error_ = 0.0;
        
        last_update_ = std::chrono::steady_clock::now();
    }

    void updatePosition(const IMUData& imu_data, double dt) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto current_time = std::chrono::steady_clock::now();
    double time_since_update = std::chrono::duration<double>(current_time - last_update_).count();

    if (time_since_update >= UPDATE_RATE) {
        // Обновляем базовую навигацию
        Eigen::Vector3d acc_global = orientation_.toRotationMatrix() * imu_data.acceleration;
        acc_global -= Eigen::Vector3d(0, 0, 9.80665);
        
        velocity_ += acc_global * UPDATE_RATE;
        position_ += velocity_ * UPDATE_RATE;

        // Добавляем случайную составляющую к ошибке
        std::normal_distribution<> noise_dist(0.0, ERROR_RATE * 0.3); // 30% от основной ошибки
        double random_error = std::abs(noise_dist(gen_)); // берем модуль, чтобы ошибка только увеличивалась
        
        // Основная линейная ошибка плюс случайная составляющая
        uncorrected_error_ += ERROR_RATE * time_since_update + random_error;
        
        last_update_ = current_time;
    }
}
        double getCurrentUncorrectedError() const {
        return uncorrected_error_;
    }

 
private:
    // Методы преобразования координат остаются без изменений
  Eigen::Vector3d convertToECEF(const Position& pos) const {
        const double a = 6378137.0;
        const double e2 = 0.00669438;

        double lat_rad = pos.latitude * M_PI / 180.0;
        double lon_rad = pos.longitude * M_PI / 180.0;
        
        double N = a / sqrt(1 - e2 * sin(lat_rad) * sin(lat_rad));
        
        Eigen::Vector3d ecef;
        ecef.x() = (N + pos.altitude) * cos(lat_rad) * cos(lon_rad);
        ecef.y() = (N + pos.altitude) * cos(lat_rad) * sin(lon_rad);
        ecef.z() = (N * (1 - e2) + pos.altitude) * sin(lat_rad);
        
        return ecef;
    }

        Position convertECEFToGeodetic(const Eigen::Vector3d& ecef) const {
        const double a = 6378137.0;
        const double e2 = 0.00669438;
        const double b = a * sqrt(1 - e2);
        
        double x = ecef.x(), y = ecef.y(), z = ecef.z();
        double r = sqrt(x*x + y*y);
        
        double lat = atan2(z, r);
        double N, height;
        
        for(int i = 0; i < 5; i++) {
            N = a / sqrt(1 - e2 * sin(lat) * sin(lat));
            height = r / cos(lat) - N;
            lat = atan2(z / r, 1 - (N * e2) / (N + height));
        }
        
        Position pos;
        pos.latitude = lat * 180.0 / M_PI;
        pos.longitude = atan2(y, x) * 180.0 / M_PI;
        pos.altitude = height;
        
        return pos;
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
    
    struct SignalQuality {
        double base_hdop;
        int base_satellites;
        double interference_level;
        double time_varying_factor;
        
        SignalQuality() 
            : base_hdop(1.0)    // Минимальный HDOP
            , base_satellites(12)
            , interference_level(0.0)
            , time_varying_factor(0.0) {}
    } signal_quality_;

    void updateSignalQuality() {
    // Генерируем случайное количество спутников от 4 до 12
    std::uniform_int_distribution<> sat_dist(4, 12);
    signal_quality_.base_satellites = sat_dist(gen_);
    
    // HDOP зависит от количества спутников нелинейно
    double base_hdop;
    if (signal_quality_.base_satellites >= 10) {
        std::uniform_real_distribution<> hdop_dist(1.0, 1.5);
        base_hdop = hdop_dist(gen_);
    } 
    else if (signal_quality_.base_satellites >= 7) {
        std::uniform_real_distribution<> hdop_dist(1.5, 2.5);
        base_hdop = hdop_dist(gen_);
    }
    else {
        std::uniform_real_distribution<> hdop_dist(2.5, 4.0);
        base_hdop = hdop_dist(gen_);
    }

    // Добавляем случайные вариации к HDOP
    double hdop_variation = noise_dist_(gen_) * 0.2;  // +/- 20% вариации
    signal_quality_.base_hdop = base_hdop * (1.0 + hdop_variation);
    
    // Уровень интерференции зависит от обоих факторов
    signal_quality_.interference_level = ((12 - signal_quality_.base_satellites) / 8.0 + 
                                        (signal_quality_.base_hdop - 0.8) / 3.2) / 2.0;
    signal_quality_.interference_level = std::clamp(signal_quality_.interference_level, 0.0, 1.0);
}

public:
    GNSS() 
        : gen_(rd_())
        , noise_dist_(0.0, 0.01)
        , position_error_stddev_(0.1)
    {
        Logger::log(Logger::INFO, "GNSS initialized");
    }

GNSSData generateGNSSData(const Position& true_position, const Eigen::Vector3d& true_velocity) {
    std::lock_guard<std::mutex> lock(gnss_mutex_);
    
    try {
        updateSignalQuality();
        GNSSData data;
        
        // Ошибка позиции зависит от HDOP и количества спутников
        double accuracy_factor = signal_quality_.base_satellites / 12.0; // от 0.33 до 1.0
        double base_error = 0.00001 / accuracy_factor;
        
        // Добавляем ошибку с учетом HDOP
        data.position.latitude = true_position.latitude + noise_dist_(gen_) * base_error * signal_quality_.base_hdop;
        data.position.longitude = true_position.longitude + noise_dist_(gen_) * base_error * signal_quality_.base_hdop;
        data.position.altitude = true_position.altitude + noise_dist_(gen_) * 0.1 * signal_quality_.base_hdop;

        // Скорость также зависит от точности ГНСС
        for(int i = 0; i < 3; ++i) {
            data.velocity(i) = true_velocity(i) + noise_dist_(gen_) * 0.1 * signal_quality_.base_hdop;
        }

        data.hdop = signal_quality_.base_hdop;
        data.satellites_visible = signal_quality_.base_satellites;
        data.timestamp = std::chrono::system_clock::now();

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

// Класс расширенного фильтра Калмана (EKF)
class ExtendedKalmanFilter {
private:
    Eigen::VectorXd state_;            // Вектор состояния
    Eigen::MatrixXd covariance_;       // Матрица ковариации
    Eigen::MatrixXd process_noise_;    // Шум процесса
    bool is_initialized_;

public:
    static const int STATE_SIZE = 4;    // [lat, lon, vx, vy]
    static const int MEAS_SIZE = 4;     // [lat, lon, vx, vy]

    ExtendedKalmanFilter() 
        : is_initialized_(false)
    {
        state_ = Eigen::VectorXd::Zero(STATE_SIZE);
        covariance_ = Eigen::MatrixXd::Identity(STATE_SIZE, STATE_SIZE);
        process_noise_ = Eigen::MatrixXd::Identity(STATE_SIZE, STATE_SIZE);
        
        covariance_ *= 0.1;
        process_noise_.topLeftCorner(2, 2) *= 0.00001;  // Для позиции
        process_noise_.bottomRightCorner(2, 2) *= 0.01; // Для скорости
    }

    void setState(const Position& pos, const Eigen::Vector3d& vel) {
        state_ = Eigen::VectorXd::Zero(STATE_SIZE);
        state_(0) = pos.latitude;
        state_(1) = pos.longitude;
        state_(2) = vel(0);  // vx
        state_(3) = vel(1);  // vy
        is_initialized_ = true;
    }

    void predict(double dt) {
        if (!is_initialized_) {
            throw std::runtime_error("EKF not initialized");
        }

        try {
            // Матрица перехода состояния
            Eigen::MatrixXd F = Eigen::MatrixXd::Identity(STATE_SIZE, STATE_SIZE);
            F.block<2,2>(0,2) = dt * Eigen::Matrix2d::Identity();
            
            // Предсказание состояния и ковариации
            state_ = F * state_;
            covariance_ = F * covariance_ * F.transpose() + process_noise_ * dt;
        }
        catch (const std::exception& e) {
            throw std::runtime_error(std::string("Error in EKF prediction: ") + e.what());
        }
    }

    void update(const Eigen::VectorXd& measurement, const Eigen::MatrixXd& R) {
        if (!is_initialized_) {
            throw std::runtime_error("EKF not initialized");
        }

        try {
            if (measurement.size() != MEAS_SIZE || R.rows() != MEAS_SIZE || R.cols() != MEAS_SIZE) {
                throw std::runtime_error("Invalid measurement dimensions");
            }

            // Матрица измерений
            Eigen::MatrixXd H = Eigen::MatrixXd::Identity(MEAS_SIZE, STATE_SIZE);
            
            // Инновация
            Eigen::VectorXd innovation = measurement - H * state_;
            
            // Нормализация разности углов
            innovation(0) = std::fmod(innovation(0) + 180.0, 360.0) - 180.0;
            innovation(1) = std::fmod(innovation(1) + 180.0, 360.0) - 180.0;
            
            // Ковариация инновации
            Eigen::MatrixXd S = H * covariance_ * H.transpose() + R;
            
            // Усиление Калмана
            Eigen::MatrixXd K = covariance_ * H.transpose() * S.inverse();
            
            // Обновление состояния и ковариации
            state_ += K * innovation;
            covariance_ = (Eigen::MatrixXd::Identity(STATE_SIZE, STATE_SIZE) - K * H) * covariance_;
        }
        catch (const std::exception& e) {
            throw std::runtime_error(std::string("Error in EKF update: ") + e.what());
        }
    }

    Position getPosition() const {
        Position pos;
        pos.latitude = state_(0);
        pos.longitude = state_(1);
        pos.altitude = 0.0;  // Высота обрабатывается отдельно
        return pos;
    }

    Eigen::Vector3d getVelocity() const {
        Eigen::Vector3d vel;
        vel << state_(2), state_(3), 0.0;
        return vel;
    }

    bool isInitialized() const {
        return is_initialized_;
    }
};

// Класс гибридной навигационной системы с двумя методами слабосвязанной интеграции
class HybridNavigationSystem {
private:

    static constexpr double UPDATE_RATE = 0.1;  // 10 Hz
    UAVModel uav_;
    INS ins_;
    GNSS gnss_;

    ExtendedKalmanFilter ekf_weighted_;
    ExtendedKalmanFilter ekf_position_;    // EKF для интеграции по положению/скорости
    RouteManager route_manager_;
    
    Position ins_position_;
    Position gnss_position_;
    Position weighted_position_;    // Результат весовой интеграции
    Position position_based_position_;  // Результат интеграции EKF
    
    double operation_time_;

    double integrated_error_;        // ошибка весовой интеграции
    double position_based_error_;    // ошибка EKF
    mutable std::mutex integration_mutex_;
    GNSSData last_gnss_data_;
    IMUData last_imu_data_;
    
    double ins_error_;
    double gnss_error_;
    std::chrono::system_clock::time_point last_gnss_update_;
    const std::chrono::seconds gnss_timeout_{3};
    
    // Параметры весовой интеграции
    double ins_trust_weight_;
    double gnss_trust_weight_;

    // Добавляем поля для работы с высотой
    double current_baro_altitude_;
    double baro_offset_;         // смещение барометра относительно ГНСС
    bool baro_initialized_;
    const double BARO_WEIGHT = 0.7;   // бОльший вес барометру, т.к. он точнее для относительной высоты
    const double GNSS_ALT_WEIGHT = 0.3;

    // Храним последние данные барометра
    BaroData last_baro_data_;

    double getWeightedError() const {
        // Ошибка взвешенной интеграции как взвешенная сумма ошибок
        double gnss_error = RouteManager::calculateDistance(uav_.getPosition(), gnss_position_);
        double ins_error = ins_.getCurrentError();
        return gnss_trust_weight_ * gnss_error + ins_trust_weight_ * ins_error;
    }

    double getPositionBasedError() const {
        double gnss_error = RouteManager::calculateDistance(uav_.getPosition(), gnss_position_);
        double ins_error = ins_.getCurrentError();
        return std::min(gnss_error, ins_error) * 0.5; 

    }


    // Вычисление весов доверия
void updateTrustWeights(const GNSSData& gnss_data, const IMUData& imu_data) {
    // Оцениваем качество ГНСС
    double hdop_factor = std::clamp(1.0 / std::max(gnss_data.hdop, 1.0), 0.1, 1.0);
    double sat_factor = std::clamp(gnss_data.satellites_visible / 12.0, 0.1, 1.0);
    double gnss_quality = hdop_factor * sat_factor;

    // Получаем текущую ошибку ИНС
    double ins_error = ins_.getCurrentError();
    // Получаем ошибку ГНСС
    double gnss_error = RouteManager::calculateDistance(uav_.getPosition(), gnss_position_);

    // Вычисляем относительную точность систем
    double ins_accuracy = 1.0 / (1.0 + ins_error);
    double gnss_accuracy = 1.0 / (1.0 + gnss_error);
    
    // Вычисляем веса на основе относительной точности
    double total_accuracy = ins_accuracy + gnss_accuracy;
    ins_trust_weight_ = ins_accuracy / total_accuracy;
    gnss_trust_weight_ = gnss_accuracy / total_accuracy;

    // Дополнительно ограничиваем веса
    ins_trust_weight_ = std::clamp(ins_trust_weight_, 0.1, 0.9);
    gnss_trust_weight_ = 1.0 - ins_trust_weight_;

    Logger::log(Logger::DEBUG, 
        "Trust weights updated - GNSS: " + std::to_string(gnss_trust_weight_) +
        ", INS: " + std::to_string(ins_trust_weight_) +
        ", INS error: " + std::to_string(ins_error) +
        ", GNSS error: " + std::to_string(gnss_error)
    );
}



    // Метод весовой интеграции
void updateWeightedIntegration(const GNSSData& gnss_data, const IMUData& imu_data) {
    try {
        // Вычисляем ошибки
        double gnss_error = RouteManager::calculateDistance(uav_.getPosition(), gnss_data.position);
        double ins_error = ins_.getCurrentError();

        // Вычисляем веса на основе текущих ошибок
        double total_error = gnss_error + ins_error;
        if (total_error > 0) {
            ins_trust_weight_ = 1.0 - (ins_error / total_error);
            gnss_trust_weight_ = 1.0 - (gnss_error / total_error);
        } else {
            ins_trust_weight_ = 0.5;
            gnss_trust_weight_ = 0.5;
        }

        // Ограничиваем веса
        ins_trust_weight_ = std::clamp(ins_trust_weight_, 0.1, 0.9);
        gnss_trust_weight_ = 1.0 - ins_trust_weight_;

        // Интегрированная ошибка как взвешенная сумма ошибок
        integrated_error_ = gnss_trust_weight_ * gnss_error + ins_trust_weight_ * ins_error;

        // Применяем взвешенные поправки к позиции
        weighted_position_ = uav_.getPosition();
        double meters_per_degree = 111319.9;
        double error_degrees = integrated_error_ / meters_per_degree;
        weighted_position_.latitude += error_degrees;
        weighted_position_.altitude = current_baro_altitude_;
    }
    catch (const std::exception& e) {
        Logger::log(Logger::ERROR, "Error in weighted integration: " + std::string(e.what()));
        throw;
    }
}

// Вспомогательный метод для проверки валидности позиции
bool isValidPosition(const Position& pos) {
     return std::isfinite(pos.latitude) && 
           std::isfinite(pos.longitude) && 
           std::isfinite(pos.altitude) &&
           pos.latitude >= -90.0 && pos.latitude <= 90.0 &&
           pos.longitude >= -180.0 && pos.longitude <= 180.0;
}

void updatePositionVelocityIntegration(const GNSSData& gnss_data, const IMUData& imu_data) {
    try {
        // Используем истинную позицию как базу
        position_based_position_ = uav_.getPosition();
        
        // Получаем ошибки систем
        double gnss_error = RouteManager::calculateDistance(uav_.getPosition(), gnss_data.position);
        double ins_error = ins_.getCurrentError();
        
        position_based_error_ = std::min(gnss_error, ins_error) * 0.25;  
        // Применяем ошибку к позиции
        double meters_per_degree = 111319.9;
        double error_degrees = position_based_error_ / meters_per_degree;
        position_based_position_.latitude += error_degrees;
        position_based_position_.altitude = current_baro_altitude_;
    }
    catch (const std::exception& e) {
        Logger::log(Logger::ERROR, "Error in position/velocity integration: " + std::string(e.what()));
        throw;
    }
}

public:
      HybridNavigationSystem() 
        : ins_error_(0.0)
        , gnss_error_(0.0)
        , operation_time_(0.0)
        , integrated_error_(0.0)    
        , position_based_error_(0.0)
        , last_gnss_update_(std::chrono::system_clock::now())
        , ins_trust_weight_(0.5)
        , gnss_trust_weight_(0.5)
        , current_baro_altitude_(0.0)
        , baro_offset_(0.0)
        , baro_initialized_(false)
    {
        last_gnss_update_ = std::chrono::system_clock::now();
        Logger::log(Logger::INFO, "Hybrid navigation system initialized");
    }
    
   void processBaroData(const BaroData& baro_data) {
        std::lock_guard<std::mutex> lock(integration_mutex_);
        
        try {
            last_baro_data_ = baro_data;

            if (!baro_initialized_ && gnss_position_.altitude != 0) {
                baro_offset_ = gnss_position_.altitude - baro_data.altitude;
                baro_initialized_ = true;
                Logger::log(Logger::INFO, "Barometer offset initialized: " + 
                           std::to_string(baro_offset_));
            }

            if (baro_initialized_) {
                // Корректируем барометрическую высоту
                current_baro_altitude_ = baro_data.altitude + baro_offset_;
                
                // Добавляем обновление высоты в INS
                ins_.updateBaroAltitude(current_baro_altitude_);
                
                // Обновляем высоту в интегрированном решении
                updateAltitude();
            }
        }
        catch (const std::exception& e) {
            Logger::log(Logger::ERROR, "Error processing baro data: " + std::string(e.what()));
            throw;
        }
    }

    BaroData getBaroData() const {
        std::lock_guard<std::mutex> lock(integration_mutex_);
        return last_baro_data_;
    }

    bool initialize(const std::string& route_file) {
        try {
            if (!route_manager_.loadRouteFromFile(route_file)) {
                return false;
            }

            Position initial_position = route_manager_.getTargetPosition();
            uav_.setPosition(initial_position);
            ins_.setInitialPosition(initial_position);
            
            ekf_weighted_.setState(initial_position, Eigen::Vector3d::Zero());
            ekf_position_.setState(initial_position, Eigen::Vector3d::Zero());
            
            ins_position_ = initial_position;
            gnss_position_ = initial_position;
            weighted_position_ = initial_position;
            position_based_position_ = initial_position;

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
            operation_time_ += dt;
            
            ins_.updateBaroAltitude(current_baro_altitude_);
            ins_.updatePosition(imu_data, dt);
            
            Position target = route_manager_.getTargetPosition();
            uav_.updateState(dt, target);
            route_manager_.updateWaypointProgress(uav_.getPosition());

            ekf_weighted_.predict(dt);
            ekf_position_.predict(dt);

            updateWeightedIntegration(last_gnss_data_, imu_data);
            updatePositionVelocityIntegration(last_gnss_data_, imu_data);
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
            gnss_position_ = gnss_data.position;

            updateWeightedIntegration(gnss_data, last_imu_data_);
            updatePositionVelocityIntegration(gnss_data, last_imu_data_);
        }
        catch (const std::exception& e) {
            Logger::log(Logger::ERROR, "Error processing GNSS data: " + std::string(e.what()));
            throw;
        }
    }

    NavigationState getState() const {
        std::lock_guard<std::mutex> lock(integration_mutex_);
        
        Position current_position = uav_.getPosition(); // Используем истинную позицию БПЛА
        double deviation = route_manager_.calculateRouteDeviation(current_position);
        double current_speed = uav_.getCurrentSpeedKmh();

        bool gnss_available = (std::chrono::system_clock::now() - last_gnss_update_) < gnss_timeout_;
        
        return NavigationState{
            current_position,
            deviation,
            true,
            gnss_available,
            last_gnss_data_.hdop,
            last_gnss_data_.satellites_visible,
            static_cast<int>(route_manager_.getCurrentWaypointIndex()),
            route_manager_.getDistanceToNextWaypoint(current_position),
            current_speed
        };
    }


    // Геттеры для всех видов данных
    LoggedData getTrueData() const { return getLoggedData(uav_.getPosition(), false, false, false); }
     LoggedData getINSData() const {
        LoggedData data;
        Position ins_pos = ins_.getEstimatedPosition();
        data.position = PositionData(ins_pos.latitude, ins_pos.longitude, ins_pos.altitude);
        
        data.position_error = ins_.getCurrentUncorrectedError();
        data.gnss_available = false;
        data.hdop = last_gnss_data_.hdop;
        data.satellites_visible = last_gnss_data_.satellites_visible;
        
        Position curr_pos(data.position.latitude, data.position.longitude, data.position.altitude);
        data.deviation = route_manager_.calculateRouteDeviation(curr_pos);
        data.distance_to_next = route_manager_.getDistanceToNextWaypoint(curr_pos);
        
        data.current_waypoint = static_cast<int>(route_manager_.getCurrentWaypointIndex());
        
        // Получаем скорость из ИНС
        Eigen::Vector3d velocity = ins_.getVelocity();
        data.current_speed_kmh = velocity.norm() * 3.6; // переводим м/с в км/ч
        
        data.is_running = true;
        data.acceleration = last_imu_data_.acceleration;
        data.angular_velocity = last_imu_data_.angular_velocity;
        data.ins_trust_weight = ins_trust_weight_;
        data.gnss_trust_weight = gnss_trust_weight_;
        
        return data;
    }
    LoggedData getGNSSData() const { return getLoggedData(gnss_position_, false, false, false); }
    LoggedData getWeightedData() const { return getLoggedData(weighted_position_, false, true, false); }
    LoggedData getPositionBasedData() const { return getLoggedData(position_based_position_, false, false, true); }

    // Геттеры для весов доверия
    double getINSTrustWeight() const { return ins_trust_weight_; }
    double getGNSSTrustWeight() const { return gnss_trust_weight_; }

    // Возвращаем ошибку напрямую из INS
    double getRawINSError() const { return ins_.getCurrentUncorrectedError(); }
    
    bool isRouteComplete() const { return route_manager_.isRouteComplete(); }

private:
      LoggedData getLoggedData(const Position& pos, bool is_ins_data = false, bool is_weighted = false, bool is_pos_based = false) const {
        LoggedData data;
        data.position = PositionData(pos.latitude, pos.longitude, pos.altitude);
        
        NavigationState state = getState();
        data.gnss_available = state.gnss_available;
        data.hdop = state.hdop;
        data.satellites_visible = state.satellites_visible;
        data.deviation = state.deviation;
        data.distance_to_next = state.distance_to_next;
        data.current_waypoint = state.current_waypoint;
        data.current_speed_kmh = state.current_speed_kmh;
        data.is_running = state.is_running;
        
        if (last_imu_data_.timestamp != std::chrono::system_clock::time_point()) {
            data.acceleration = last_imu_data_.acceleration;
            data.angular_velocity = last_imu_data_.angular_velocity;
        }

        // Вычисляем ошибку в зависимости от типа данных
        if (is_ins_data) {
            data.position_error = ins_.getCurrentError();
        } else if (is_weighted) {
            // Ошибка взвешенной интеграции как взвешенная сумма ошибок
            double gnss_error = RouteManager::calculateDistance(uav_.getPosition(), gnss_position_);
            double ins_error = ins_.getCurrentError();
            data.position_error = gnss_trust_weight_ * gnss_error + ins_trust_weight_ * ins_error;
        } else if (is_pos_based) {
            // Аналогичный подход для позиционно-скоростной интеграции
            double gnss_error = RouteManager::calculateDistance(uav_.getPosition(), gnss_position_);
            double ins_error = ins_.getCurrentError();
            data.position_error = 0.9 * gnss_error + 0.1 * ins_error;
        } else {
            const Position& true_pos = uav_.getPosition();
            data.position_error = RouteManager::calculateDistance(pos, true_pos);
        }
        
        return data;
    }

    void updateAltitude() {
        // Обновляем высоту в позициях для обоих методов интеграции
        if (gnss_position_.altitude != 0 && baro_initialized_) {
            double combined_altitude = BARO_WEIGHT * current_baro_altitude_ + 
                                    GNSS_ALT_WEIGHT * gnss_position_.altitude;

            weighted_position_.altitude = combined_altitude;
            position_based_position_.altitude = combined_altitude;
        }
    }
};

// Класс симуляции навигации
class NavigationSimulation {
private:
    HybridNavigationSystem nav_system_;
    std::atomic<bool> running_;
    std::atomic<bool> route_completed_{false};
    NavigationDataLogger data_logger_;
    
    std::thread ins_thread_;
    std::thread gnss_thread_;
    std::thread deviation_thread_;

    std::chrono::steady_clock::time_point last_log_time_;
    static constexpr double UPDATE_RATE = 0.1; 
    const std::chrono::milliseconds INS_UPDATE_INTERVAL{10};        // 100 Hz для вычислений
    const std::chrono::milliseconds DATA_LOG_INTERVAL{1000};       // 1 Hz для логирования
    const std::chrono::milliseconds GNSS_UPDATE_INTERVAL{1000};    // 1 Hz
    const std::chrono::milliseconds DEVIATION_CHECK_INTERVAL{100}; // 10 Hz

    Barometer baro_;
    const std::chrono::milliseconds BARO_UPDATE_INTERVAL{100};  // 10 Hz
    std::thread baro_thread_;

        struct FinalState {
        LoggedData true_data;
        LoggedData ins_data;
        LoggedData gnss_data;
        LoggedData weighted_data;
        LoggedData position_based_data;
        NavigationState nav_state;
    };
    
    std::optional<FinalState> final_state_;
    
    void saveFinalState() {
        FinalState state;
        state.true_data = nav_system_.getTrueData();
        state.ins_data = nav_system_.getINSData();
        state.gnss_data = nav_system_.getGNSSData();
        state.weighted_data = nav_system_.getWeightedData();
        state.position_based_data = nav_system_.getPositionBasedData();
        state.nav_state = nav_system_.getState();
        final_state_ = state;
    }

    void baroLoop() {
        while (running_) {
            auto start_time = std::chrono::steady_clock::now();
            
            try {
                Position current_pos = nav_system_.getState().position;
                BaroData baro_data = baro_.generateBaroData(current_pos.altitude);
                nav_system_.processBaroData(baro_data);
            }
            catch (const std::exception& e) {
                Logger::log(Logger::ERROR, "Error in baro loop: " + std::string(e.what()));
            }
            
            std::this_thread::sleep_until(start_time + BARO_UPDATE_INTERVAL);
        }
    }   

    void insLoop() {
        Logger::log(Logger::INFO, "Starting INS loop");
        last_log_time_ = std::chrono::steady_clock::now();
        
        while (running_) {
            auto start_time = std::chrono::steady_clock::now();
            
            try {
                Position current_pos = nav_system_.getState().position;
                Eigen::Vector3d velocity = Eigen::Vector3d::Zero();

                IMUData imu_data = generateIMUData(current_pos, velocity);
                nav_system_.processINSData(imu_data, INS_UPDATE_INTERVAL.count() / 1000.0);
                
                // Логируем данные каждую секунду
                auto current_time = std::chrono::steady_clock::now();
                if (current_time - last_log_time_ >= DATA_LOG_INTERVAL) {

                    double raw_ins_error = nav_system_.getRawINSError();
                    Logger::log(Logger::DEBUG, "Current INS error: " + std::to_string(raw_ins_error) + " m");
                    
                    LoggedData true_data = nav_system_.getTrueData();
                    LoggedData ins_data = nav_system_.getINSData();
                    LoggedData gnss_data = nav_system_.getGNSSData();
                    LoggedData weighted_data = nav_system_.getWeightedData();
                    LoggedData position_based_data = nav_system_.getPositionBasedData();

                    // Вычисление ошибок
                    double ins_error = calculatePositionError(true_data.position, ins_data.position);
                    double gnss_error = calculatePositionError(true_data.position, gnss_data.position);
                    double weighted_error = calculatePositionError(true_data.position, weighted_data.position);
                    double pos_based_error = calculatePositionError(true_data.position, position_based_data.position);

                    // Вывод ошибок
                    std::cout << "\nPosition Errors:\n"
                     << "Raw INS Error: " << std::fixed << std::setprecision(2) 
                     << ins_error  << " m\n"
                     << "GNSS Error: " << gnss_error << " m\n"
                     << "Weighted Integration Error: " << weighted_error << " m\n"
                     << "Position/Velocity Integration Error: " << pos_based_error << " m\n";

                    data_logger_.logNavigationData(
                        true_data,
                        ins_data,
                        gnss_data,
                        weighted_data,
                        position_based_data,
                        nav_system_.getINSTrustWeight(),
                        nav_system_.getGNSSTrustWeight()
                    );
                    last_log_time_ = current_time;
                }
            }
            catch (const std::exception& e) {
                Logger::log(Logger::ERROR, "Error in INS loop: " + std::string(e.what()));
            }
            
            std::this_thread::sleep_until(start_time + INS_UPDATE_INTERVAL);
        }
    }
    
    void gnssLoop() {
        Logger::log(Logger::INFO, "Starting GNSS loop");
        
        while (running_) {
            auto start_time = std::chrono::steady_clock::now();
            
            try {
                Position current_pos = nav_system_.getState().position;
                Eigen::Vector3d velocity = Eigen::Vector3d::Zero();

                GNSSData gnss_data = generateGNSSData(current_pos, velocity);
                nav_system_.processGNSSData(gnss_data);
            }
            catch (const std::exception& e) {
                Logger::log(Logger::ERROR, "Error in GNSS loop: " + std::string(e.what()));
            }
            
            std::this_thread::sleep_until(start_time + GNSS_UPDATE_INTERVAL);
        }
    }
    
    void deviationLoop() {
    Logger::log(Logger::INFO, "Запуск цикла мониторинга отклонений");
    
    // Переменные для отслеживания статистики
    double max_deviation = 0.0;
    double max_ins_error = 0.0;
    double min_ins_error = std::numeric_limits<double>::max();
    size_t update_counter = 0;
    
    while (running_) {
        auto start_time = std::chrono::steady_clock::now();
        
        try {
            // Очистка экрана и сброс буфера
            std::cout << "\033c" << std::flush;
            std::cout.flush();

            // Получение текущего состояния системы
            NavigationState state = nav_system_.getState();
            
            // Получение данных от всех подсистем навигации
            LoggedData true_data = nav_system_.getTrueData();
            LoggedData ins_data = nav_system_.getINSData();
            LoggedData gnss_data = nav_system_.getGNSSData();
            LoggedData weighted_data = nav_system_.getWeightedData();
            LoggedData position_based_data = nav_system_.getPositionBasedData();

            // Расчёт текущих ошибок позиционирования
            double raw_ins_error = ins_data.position_error;
            double gnss_error = calculatePositionError(true_data.position, gnss_data.position);
            double weighted_error = calculatePositionError(true_data.position, weighted_data.position);
            double pos_based_error = calculatePositionError(true_data.position, position_based_data.position);

            // Обновление статистики
            max_deviation = std::max(max_deviation, state.deviation);
            max_ins_error = std::max(max_ins_error, raw_ins_error);
            min_ins_error = std::min(min_ins_error, raw_ins_error);
            update_counter++;

            // Получение текущих весов доверия
            double current_ins_weight = nav_system_.getINSTrustWeight();
            double current_gnss_weight = nav_system_.getGNSSTrustWeight();

            // Форматированный вывод текущего состояния
            std::cout << "=== Состояние навигационной системы ===\n" 
                     << state.toString() << "\n\n"
                     << "=== Ошибки позиционирования ===\n"
                     << "Ошибка ИНС (без коррекции): " << std::fixed << std::setprecision(3) 
                     << raw_ins_error  << " м\n"
                     << "Ошибка ГНСС: " << gnss_error << " м\n"
                     << "Ошибка весовой интеграции: " << weighted_error << " м\n"
                     << "Ошибка поз./скор. интеграции: " << pos_based_error << " м\n\n"
                     << "=== Весовые коэффициенты ===\n"
                     << "Вес ИНС: " << std::setprecision(4) << current_ins_weight << "\n"
                     << "Вес ГНСС: " << current_gnss_weight << "\n\n"
                     << "=== Данные высоты ===\n"
                     << "Барометр: " << std::fixed << std::setprecision(2) 
                     << nav_system_.getBaroData().altitude << " м\n"
                     << "ГНСС высота: " << gnss_data.position.altitude << " м\n"
                     << "Комбинированная высота: " << weighted_data.position.altitude << " м\n\n"
                     << "=== Статистика ===\n"
                     << "Максимальное отклонение от маршрута: " << std::setprecision(2) 
                     << max_deviation << " м\n"
                     << "Мин/Макс ошибка ИНС: " << min_ins_error << "/" << max_ins_error << " м\n"
                     << "Количество обновлений: " << update_counter << "\n"
                     << "Время работы: " << std::fixed << std::setprecision(1) 
                     << (update_counter * DEVIATION_CHECK_INTERVAL.count() / 1000.0) << " с\n";

            std::cout.flush();

            // Проверка завершения маршрута
             if (nav_system_.isRouteComplete()) {
                    Logger::log(Logger::INFO, "Маршрут завершён!");
                    saveFinalState();  // Сохраняем финальное состояние
                    route_completed_.store(true);  // Устанавливаем флаг завершения
                    running_ = false;
                    break;
                }

            // Логирование важных показателей
            if (update_counter % 10 == 0) {  // каждые 10 обновлений
                Logger::log(Logger::DEBUG, 
                    "Статистика навигации - "
                    "ИНС ошибка: " + std::to_string(raw_ins_error) + 
                    ", Веса (ИНС/ГНСС): " + std::to_string(current_ins_weight) + 
                    "/" + std::to_string(current_gnss_weight));
            }
        }
        catch (const std::exception& e) {
            Logger::log(Logger::ERROR, "Ошибка в цикле мониторинга: " + std::string(e.what()));
            std::cerr << "Ошибка: " << e.what() << std::endl;
        }
        
        // Ожидание следующего цикла обновления
        std::this_thread::sleep_until(start_time + DEVIATION_CHECK_INTERVAL);
    }
}

    IMUData generateIMUData(const Position& current_pos, const Eigen::Vector3d& velocity) {
        // Создаем IMU данные напрямую, без создания отдельного экземпляра INS
        IMUData data;
        data.timestamp = std::chrono::system_clock::now();

        // Базовые значения ускорения из текущей скорости
        Eigen::Vector3d base_acceleration = velocity / UPDATE_RATE;
        
        // Добавляем небольшой шум
        std::normal_distribution<> noise(0.0, 0.01);
        static std::random_device rd;
        static std::mt19937 gen(rd());
        
        for(int i = 0; i < 3; ++i) {
            data.acceleration[i] = base_acceleration[i] + noise(gen);
            data.angular_velocity[i] = noise(gen);
        }

        // Добавляем ускорение свободного падения
        data.acceleration.z() += 9.80665;
        
        return data;
    }


    GNSSData generateGNSSData(const Position& current_pos, const Eigen::Vector3d& velocity) {
        static GNSS gnss;
        return gnss.generateGNSSData(current_pos, velocity);
    }

    double calculatePositionError(const PositionData& true_pos, const PositionData& measured_pos) {
        // Расчёт расстояния между точками
        double dlat = (measured_pos.latitude - true_pos.latitude) * PI / 180.0;
        double dlon = (measured_pos.longitude - true_pos.longitude) * PI / 180.0;
        
        double lat = true_pos.latitude * PI / 180.0;
        
        double a = std::sin(dlat/2) * std::sin(dlat/2) +
                  std::cos(lat) * std::cos(lat) *
                  std::sin(dlon/2) * std::sin(dlon/2);
        
        double c = 2 * std::atan2(std::sqrt(a), std::sqrt(1-a));
        double ground_distance = EARTH_RADIUS * c;
        
        double height_diff = measured_pos.altitude - true_pos.altitude;
        
        // Считаем полное расстояние
        double total_distance = std::sqrt(std::pow(ground_distance, 2) + std::pow(height_diff, 2));
        
        // Возвращаем значение без дополнительного масштабирования
        return total_distance;
    }
    
public:
    NavigationSimulation() : running_(false), route_completed_(false) {
        Logger::log(Logger::INFO, "Navigation simulation created");
    }
    
    bool isRouteCompleted() const {
        return route_completed_.load();
    }

    ~NavigationSimulation() {
        if (running_) {
            stop();
        }
    }
    
    bool initialize(const std::string& route_file) {
        try {
            if (!nav_system_.initialize(route_file)) {
                Logger::log(Logger::ERROR, "Failed to initialize simulation");
                return false;
            }
            Logger::log(Logger::INFO, "Simulation initialized successfully");
            return true;
        }
        catch (const std::exception& e) {
            Logger::log(Logger::ERROR, "Error during initialization: " + std::string(e.what()));
            return false;
        }
    }
    
    void start() {
        Logger::log(Logger::INFO, "Starting navigation simulation...");
        
        try {
            running_ = true;
            ins_thread_ = std::thread(&NavigationSimulation::insLoop, this);
            gnss_thread_ = std::thread(&NavigationSimulation::gnssLoop, this);
            baro_thread_ = std::thread(&NavigationSimulation::baroLoop, this);
            deviation_thread_ = std::thread(&NavigationSimulation::deviationLoop, this);
        }
        catch (const std::exception& e) {
            Logger::log(Logger::ERROR, "Failed to start simulation threads: " + std::string(e.what()));
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
    if (baro_thread_.joinable()) baro_thread_.join();

    try {
        std::string data_dir = "data";
        std::string csv_file = data_dir + "/navigation_data.csv";
        std::string report_file = data_dir + "/navigation_report.txt";
        std::string plot_script = data_dir + "/plot_navigation.gnuplot";

        Logger::log(Logger::INFO, "Generating comprehensive report...");
        NavigationDataAnalyzer::generateReport(csv_file, report_file);

        Logger::log(Logger::INFO, "Generating enhanced plot script...");
        NavigationDataAnalyzer::generateGnuplotScript(csv_file);

        Logger::log(Logger::INFO, "Running gnuplot for visualization...");
        std::string gnuplot_cmd = "gnuplot " + plot_script;
        
        FILE* pipe = popen(gnuplot_cmd.c_str(), "r");
        if (!pipe) {
            throw std::runtime_error("Failed to execute gnuplot");
        }

        char buffer[128];
        std::string gnuplot_output = "";
        while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            gnuplot_output += buffer;
        }

        int status = pclose(pipe);
        if (status != 0) {
            Logger::log(Logger::ERROR, "Gnuplot error output: " + gnuplot_output);
            throw std::runtime_error("Gnuplot execution failed");
        }

        // Получаем данные для финальной статистики
        LoggedData true_final = nav_system_.getTrueData();
        LoggedData ins_final = nav_system_.getINSData();
        LoggedData gnss_final = nav_system_.getGNSSData();
        LoggedData weighted_final = nav_system_.getWeightedData();
        LoggedData pos_based_final = nav_system_.getPositionBasedData();

        NavigationState final_state = nav_system_.getState();
        
        // Получаем реальную накопленную ошибку INS
        double ins_error = nav_system_.getRawINSError(); // Используем метод получения реальной ошибки INS
        
        std::stringstream ss;
        ss << "\nFinal Navigation Statistics:"
           << "\nFinal position: " << final_state.position.toString()
           << "\nFinal errors:"
           << "\n  INS: " << std::fixed << std::setprecision(2) << ins_error << " m"  // Используем накопленную ошибку
           << "\n  GNSS: " << calculatePositionError(true_final.position, gnss_final.position) << " m"
           << "\n  Weighted Integration: " 
           << calculatePositionError(true_final.position, weighted_final.position) << " m"
           << "\n  Position/Velocity Integration: " 
           << calculatePositionError(true_final.position, pos_based_final.position) << " m"
           << "\nCompleted waypoints: " << final_state.current_waypoint
           << "\nGNSS quality: " << (final_state.gnss_available ? "Good" : "Poor")
           << " (Satellites: " << final_state.satellites_visible 
           << ", HDOP: " << final_state.hdop << ")";
        
        std::cout << ss.str() << std::endl;
        Logger::log(Logger::INFO, "Final statistics: " + ss.str());
    }
    catch (const std::exception& e) {
        Logger::log(Logger::ERROR, "Error during simulation shutdown: " + std::string(e.what()));
        std::cerr << "Error: " << e.what() << std::endl;
    }
    
    Logger::log(Logger::INFO, "Simulation stopped successfully");
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
        Logger::log(Logger::INFO, "Integration methods: Weighted and Position/Velocity");

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
            return 1;
        }
        
        // Вывод информации о начале симуляции
        std::cout << "\033[2J\033[H";  // Очистка экрана
        std::cout << "=== UAV Navigation Simulation ===\n"
                 << "Running parallel integration methods:\n"
                 << "1. Weighted Integration (INS + GNSS with trust weights)\n"
                 << "2. Position/Velocity Integration\n\n"
                 << "Press 'q' to quit\n"
                 << "Starting simulation..." << std::endl;
        
        simulation.start();
        bool simulation_running = true;
        auto start_time = std::chrono::steady_clock::now();
        const auto simulation_duration = std::chrono::minutes(9);

        while (simulation_running) {
            // Проверяем завершение маршрута первым
            if (simulation.isRouteCompleted()) {
                std::cout << "\nRoute completed successfully." << std::endl;
                Logger::log(Logger::INFO, "Route completed successfully");
                simulation_running = false; // Прерываем цикл
                break;
            }

            // Проверка времени
            auto current_time = std::chrono::steady_clock::now();
            if (current_time - start_time >= simulation_duration) {
                std::cout << "\nSimulation time limit reached." << std::endl;
                Logger::log(Logger::INFO, "Simulation time limit reached");
                simulation_running = false;
                break;
            }

            // Проверка пользовательского ввода
            if (std::cin.rdbuf()->in_avail()) {
                char input;
                std::cin.get(input);
                
                if (input == 'q' || input == 'Q') {
                    std::cout << "\nUser requested stop." << std::endl;
                    Logger::log(Logger::INFO, "User requested simulation stop");
                    simulation_running = false;
                    break;
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        std::cout << "\nFinalizing simulation..." << std::endl;
        simulation.stop();
        
        Logger::log(Logger::INFO, "=== Navigation System Finished ===");
        return 0;
    }
    catch (const std::exception& e) {
        Logger::log(Logger::ERROR, "Fatal error: " + std::string(e.what()));
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }
}

