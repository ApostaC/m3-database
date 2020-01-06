#include <functional>
#include <algorithm>
#include "CarrierEnv.hh"
#include "../common/Progress.hh"

using Loc_t = std::pair<double, double>;    // first is lng, second is lat
/* ===== helpers ===== */
/**
 * getDistance, return the estimated distance (meters) between two location
 */
static double getDistance(const Loc_t &l1, const Loc_t &l2);

/**
 * findNearest, find the nearest location in a dataframe
 */
static src::Datablock findNearest(const Loc_t &loc, const src::DataFrame &df);

/**
 * struct LocCmp
 * used to compare the distance between a specified location
 */
struct LocCmp
{
    Loc_t point;
    LocCmp(const Loc_t &loc) : point(loc) {}
    bool operator()(const Loc_t &l, const Loc_t &r) const
    {
        return getDistance(l, point) < getDistance(r, point);
    }
};


namespace src
{

CarrierEnv::CarrierEnv(const std::vector<std::string> &files)
{
    using lb = src::Label;
    LOG_DEBUG("CarrierEnv: Got ", files.size(), "data files");
    common::ProgressBar bar(std::cerr, files.size(), "Reading");
    for(auto &file: files)
    {
        bar.increase(1);
        df.emplace_back(file);
        df.back().setLabels({lb::INDEX, lb::LONGTITUDE, lb::LATITIDE, lb::SPEED,
                lb::THROUGHPUT, lb::RTT, lb::LOSS, lb::RSRP, lb::TIME,
                lb::HANDOVER, lb::CELLID});
    }

    this->prediction.setLabels({lb::TIME, lb::THROUGHPUT, lb::RTT, lb::LOSS, lb::HANDOVER});
    current_cell = 0;
}

void CarrierEnv::updateCell(Cid_t new_cell)
{
    this->current_cell = new_cell;
}

DataFrame CarrierEnv::getPrediction()
{
    DataFrame ret;
    {
        std::unique_lock<std::mutex> lock(this->pred_lock);
        ret = this->prediction;
    }
    return this->prediction;
}

void CarrierEnv::updateLocation(double lng, double lat, double time)
{
    LOG_DEBUG("CarrierEnv::updateLocation", lng, lat, time, "df size is", df.size());
    const static double SAME_LOCATION_THRESHOLD = 100; // if more than 100m, 2 locations are different location
    const static double MATCH_LENGTH = 5;   // get [t, t+5)
    /* for each day, get the nearest location point */
    /* for each day, get the [0, 5] second data */
    /* collect all of them into one DataFrame, add a column "day" */
    using lb = src::Label;

    Loc_t curr_loc{lng, lat};
    Cid_t curr_cell = this->current_cell;

    /* dataFrame for all matched data */
    DataFrame allFrame;
    allFrame.setLabels(this->df[0].getLabels());
    allFrame.addColumn("day", 0);

    for(unsigned i = 0; i < df.size(); i++) // for each day
    {
        auto &curr_df = this->df[i];
        auto col_lng = curr_df.getColumn(lb::LONGTITUDE),
             col_lat = curr_df.getColumn(lb::LATITIDE),
             col_time = curr_df.getColumn(lb::TIME),
             col_cell = curr_df.getColumn(lb::CELLID);

        auto block = curr_df.where([=](const Datablock &b)
                {
                    auto ln = b.get(col_lng),
                         la = b.get(col_lat);
                    return std::fabs(lng - ln) < 0.2 && std::fabs(lat - la) < 0.2;
                });

        /* now, linear search for the nearest point in block, and get its time t */
        if (block.rows() == 0) continue;    // skip if no data
        //LOG_DEBUG("small block size is: ", block.rows());
        auto nearest_record = findNearest(curr_loc, block); // it is the nearest point
        Loc_t nearest_loc{nearest_record.get(col_lng), nearest_record.get(col_lat)};
        if(getDistance(nearest_loc, curr_loc) > SAME_LOCATION_THRESHOLD 
                || nearest_record.get(col_cell) != curr_cell)
            continue; // if too far-away or not in one cell, we skip it
        auto start_time = nearest_record.get(col_time);

        /* now, get the [t, t+5) second data */
        auto next_5sec = curr_df.where([start_time, col_time](const Datablock &b)
                {
                    return //b.get(col_cell) == curr_cell && 
                        b.get(col_time) < start_time + MATCH_LENGTH &&
                        b.get(col_time) >= start_time;
                });

        LOG_DEBUG("next 5 sec size is: ", next_5sec.rows());

        /* change the time from [t, t+5) to [0, 5) */
        auto &temp_vec = next_5sec.getData();
        for(auto &db : temp_vec) 
            db.set(col_time, db.get(col_time) - start_time);

        /* add the column: "day" */
        next_5sec.addColumn("day", i);
        allFrame.extend(next_5sec);
    }

    LOG_DEBUG("allFrame is:");
    std::cerr<<allFrame.to_string()<<std::endl;


    /* now, we should make prediction based on allFrame */
    /* thp[0:5] = daily average thp[0:5], Byte per sec*/
    /* rtt[0:5] = daily average ..., second*/
    /* loss[0:5] = daily average ..., 0~1 */
    /* handver[0:5] = daily average ... (1 means always handover, 0 means no handover) */
    DataFrame temp_pred;
    temp_pred.setLabels(this->prediction.getLabels());

    for(int t = 0; t < MATCH_LENGTH; t++)   // for each second
    {
        temp_pred.addRow();
        temp_pred.getData().back().set(temp_pred.getColumn(lb::TIME), time + t); // update time

        auto time_col = allFrame.getColumn(lb::TIME);
        auto tdf = allFrame.where(time_col, [t](double v){return std::round(v) == t;})
                           .select({lb::THROUGHPUT, lb::RTT, lb::LOSS, lb::HANDOVER});

        for(auto label : tdf.getLabels())   // for each type
        {
            auto label_df = tdf.select({label});
            double sum = 0;
            for(auto &record : label_df.getData()) 
            {
                if(label == lb::HANDOVER)
                    sum += !!((int)record.get(0));  // convert 0~5 value to binary value (0 or 1)
                else
                    sum += record.get(0);
            }
            sum /= label_df.rows();
            temp_pred.getData().back().set(temp_pred.getColumn(label), sum); //update the average
        }
    }

    /* lock and update the label */
    {
        std::unique_lock<std::mutex> lock(this->pred_lock);
        this->prediction = temp_pred;
    }
}

}   // namespace src

/* helpers */
static double getDistance(const Loc_t &l1, const Loc_t &l2)
{
    const double r = 6371.0 * 1000; // in meter
    const double pi = 2 * std::asin(1.);
    auto delta_lng = l1.first - l2.first;
    auto delta_lat = l1.second - l2.second;
    auto y = 2 * pi * r * delta_lat / 360;
    auto x = 2 * pi * (r * std::cos(pi * l1.second / 180.)) * delta_lng / 360;
    return std::sqrt(x * x + y * y);
}

static src::Datablock findNearest(const Loc_t &loc, const src::DataFrame &df)
{
    src::DataFrame ret;
    ret.setLabels(df.getLabels());
    
    std::vector<Loc_t> vec;
    auto col1 = df.getColumn(src::Label::LONGTITUDE),
         col2 = df.getColumn(src::Label::LATITIDE);

    for(auto &v : df.getData())
        vec.push_back({v.get(col1), v.get(col2)});

    std::sort(vec.begin(), vec.end(), LocCmp(loc));
    auto &target = vec[0];

    return df.where([target, col1, col2](const src::Datablock &b)
            {
                return b.get(col1) == target.first && b.get(col2) == target.second;
            }).getData()[0];
}
