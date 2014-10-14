/*****************************************
*  Standalone ISMRMRD Gadgetron Client  
*
* Author: Michael S. Hansen
* 
* Dependencies: ISMRMRD and Boost
*
*****************************************/

//TODO:
// -Blobs (for DICOM image support)
//  - First implementation is in, but testing needed
// -NIFTI and Analyze output
// -Check on potential threading problem with asio socket 
//    - having and reading and writing thread is supposedly not safe, but seems to work here
// -Add command line switch for controlling verbosity of output
// -Static linking for standalone executable. 

#include <boost/program_options.hpp>
#include <boost/asio.hpp>
#include <boost/thread/thread.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/shared_ptr.hpp>

#include <ismrmrd/ismrmrd.h>
#include <ismrmrd/dataset.h>

#include <fstream>
#include <streambuf>
#include <time.h>
#include <iomanip>
#include <sstream>
#include <iostream>
#include <exception>
#include <map>


std::string get_date_time_string()
{
    time_t rawtime;
    struct tm * timeinfo;
    time ( &rawtime );
    timeinfo = localtime ( &rawtime );

    std::stringstream str;
    str << timeinfo->tm_year+1900 << "-"
        << std::setw(2) << std::setfill('0') << timeinfo->tm_mon+1 << "-"
        << std::setw(2) << std::setfill('0') << timeinfo->tm_mday << " "
        << std::setw(2) << std::setfill('0') << timeinfo->tm_hour << ":"
        << std::setw(2) << std::setfill('0') << timeinfo->tm_min << ":"
        << std::setw(2) << std::setfill('0') << timeinfo->tm_sec;

    std::string ret = str.str();

    return ret;
}


namespace po = boost::program_options;
using boost::asio::ip::tcp;


enum GadgetronMessageID {
    GADGET_MESSAGE_INT_ID_MIN                             =   0,
    GADGET_MESSAGE_CONFIG_FILE                            =   1,
    GADGET_MESSAGE_CONFIG_SCRIPT                          =   2,
    GADGET_MESSAGE_PARAMETER_SCRIPT                       =   3,
    GADGET_MESSAGE_CLOSE                                  =   4,
    GADGET_MESSAGE_INT_ID_MAX                             = 999,
    GADGET_MESSAGE_EXT_ID_MIN                             = 1000,
    GADGET_MESSAGE_ACQUISITION                            = 1001, /**< DEPRECATED */
    GADGET_MESSAGE_NEW_MEASUREMENT                        = 1002, /**< DEPRECATED */
    GADGET_MESSAGE_END_OF_SCAN                            = 1003, /**< DEPRECATED */
    GADGET_MESSAGE_IMAGE_CPLX_FLOAT                       = 1004, /**< DEPRECATED */
    GADGET_MESSAGE_IMAGE_REAL_FLOAT                       = 1005, /**< DEPRECATED */
    GADGET_MESSAGE_IMAGE_REAL_USHORT                      = 1006, /**< DEPRECATED */
    GADGET_MESSAGE_EMPTY                                  = 1007, /**< DEPRECATED */
    GADGET_MESSAGE_ISMRMRD_ACQUISITION                    = 1008,
    GADGET_MESSAGE_ISMRMRD_IMAGE_CPLX_FLOAT               = 1009,
    GADGET_MESSAGE_ISMRMRD_IMAGE_REAL_FLOAT               = 1010,
    GADGET_MESSAGE_ISMRMRD_IMAGE_REAL_USHORT              = 1011,
    GADGET_MESSAGE_DICOM                                  = 1012,
    GADGET_MESSAGE_CLOUD_JOB                              = 1013,
    GADGET_MESSAGE_GADGETCLOUD_JOB                        = 1014,
    GADGET_MESSAGE_ISMRMRD_IMAGEWITHATTRIB_CPLX_FLOAT     = 1015,
    GADGET_MESSAGE_ISMRMRD_IMAGEWITHATTRIB_REAL_FLOAT     = 1016,
    GADGET_MESSAGE_ISMRMRD_IMAGEWITHATTRIB_REAL_USHORT    = 1017,
    GADGET_MESSAGE_DICOM_WITHNAME                         = 1018,
    GADGET_MESSAGE_DEPENDENCY_QUERY                       = 1019,
    GADGET_MESSAGE_EXT_ID_MAX                             = 4096
};

boost::mutex mtx;

struct GadgetMessageIdentifier
{
    uint16_t id;
};

struct GadgetMessageConfigurationFile
{
    char configuration_file[1024];
};

struct GadgetMessageScript
{
    uint32_t script_length;
};

class GadgetronClientException : public std::exception
{

public:
    GadgetronClientException(std::string msg)
        : msg_(msg)
    {

    }

    virtual ~GadgetronClientException() throw() {}

    virtual const char* what() const throw()
    {
        return msg_.c_str();
    }

protected:
    std::string msg_;
};

class GadgetronClientMessageReader
{
public:
    virtual ~GadgetronClientMessageReader() {}

    /**
    Function must be implemented to read a specific message.
    */
    virtual void read(tcp::socket* s) = 0;

};


template <typename T> class GadgetronClientImageMessageReader 
    : public GadgetronClientMessageReader
{

public:
    GadgetronClientImageMessageReader(std::string filename, std::string groupname)
        : file_name_(filename)
        , group_name_(groupname)
    {

    }

    ~GadgetronClientImageMessageReader() {
    } 

    virtual void read(tcp::socket* stream) 
    {
        //std::cout << "Receiving image." << std::endl;
        //Read the image from the socket
        ISMRMRD::ImageHeader h;
        boost::asio::read(*stream, boost::asio::buffer(&h,sizeof(ISMRMRD::ImageHeader)));

        // TODO check the datatype!
        ISMRMRD::Image<T> im; 
        im.setHead(h);
        boost::asio::read(*stream, boost::asio::buffer(im.getData(), im.getDataSize()));
        {
            if (!dataset_) {

                {
                    mtx.lock();
                    dataset_ = boost::shared_ptr<ISMRMRD::Dataset>(new ISMRMRD::Dataset(file_name_.c_str(), group_name_.c_str(), true)); // create if necessary 
                    mtx.unlock();
                }
            }

            std::stringstream st1;
            st1 << "image_" << h.image_series_index;
            std::string image_varname = st1.str();

            {
                mtx.lock();
                // TODO should this be wrapped in a try/catch?
                dataset_->appendImage(image_varname, ISMRMRD::ISMRMRD_BLOCKMODE_ARRAY, im);
                mtx.unlock();
            }

        }
    }

protected:
    std::string group_name_;
    std::string file_name_;
    boost::shared_ptr<ISMRMRD::Dataset> dataset_;
};

template <typename T> class GadgetronClientAttribImageMessageReader 
    : public GadgetronClientMessageReader
{

public:
    GadgetronClientAttribImageMessageReader(std::string filename, std::string groupname)
        : file_name_(filename)
        , group_name_(groupname)
    {

    }

    ~GadgetronClientAttribImageMessageReader() {
    } 

    virtual void read(tcp::socket* stream) 
    {
        //std::cout << "Receiving image with attributes." << std::endl;
        //Read the image headerfrom the socket
        ISMRMRD::ImageHeader h;
        boost::asio::read(*stream, boost::asio::buffer(&h,sizeof(ISMRMRD::ImageHeader)));
        ISMRMRD::Image<T> im;
        im.setHead(h);

        typedef unsigned long long size_t_type;

        //Read meta attributes
        size_t_type meta_attrib_length;
        boost::asio::read(*stream, boost::asio::buffer(&meta_attrib_length, sizeof(size_t_type)));

        std::string meta_attrib(meta_attrib_length,0);
        boost::asio::read(*stream, boost::asio::buffer(const_cast<char*>(meta_attrib.c_str()), meta_attrib_length));
        im.setAttributeString(meta_attrib);

        //Read image data
        boost::asio::read(*stream, boost::asio::buffer(im.getData(), im.getDataSize()));
        {
            if (!dataset_) {

                {
                    mtx.lock();
                    dataset_ = boost::shared_ptr<ISMRMRD::Dataset>(new ISMRMRD::Dataset(file_name_.c_str(), group_name_.c_str(), true)); // create if necessary 
                    mtx.unlock();
                }
            }

            std::stringstream st1;
            st1 << "image_" << h.image_series_index;
            std::string image_varname = st1.str();

            {
                mtx.lock();
                //TODO should this be wrapped in a try/catch?
                dataset_->appendImage(image_varname, ISMRMRD::ISMRMRD_BLOCKMODE_ARRAY, im);
                mtx.unlock();
            }
        }
    }

protected:
    std::string group_name_;
    std::string file_name_;
    boost::shared_ptr<ISMRMRD::Dataset> dataset_;
};


#define MAX_BLOBS_LOG_10    6

class GadgetronClientBlobMessageReader 
    : public GadgetronClientMessageReader
{

public:
    GadgetronClientBlobMessageReader(std::string fileprefix, std::string filesuffix)
        : number_of_calls_(0)
        , file_prefix(fileprefix)
        , file_suffix(filesuffix)

    {

    }

    virtual ~GadgetronClientBlobMessageReader() {}

    virtual void read(tcp::socket* socket) 
    {

        // MUST READ 32-bits
        uint32_t nbytes;
        boost::asio::read(*socket, boost::asio::buffer(&nbytes,sizeof(uint32_t)));

        std::vector<char> data(nbytes,0);
        boost::asio::read(*socket, boost::asio::buffer(&data[0],nbytes));

        std::stringstream filename;

        // Create the filename: (prefix_%06.suffix)
        filename << file_prefix << "_";
        filename << std::setfill('0') << std::setw(MAX_BLOBS_LOG_10) << number_of_calls_;
        filename << "." << file_suffix;

        std::ofstream outfile;
        outfile.open (filename.str().c_str(), std::ios::out|std::ios::binary);

        std::cout << "Writing image " << filename.str() << std::endl;

        if (outfile.good()) {
            /* write 'size' bytes starting at 'data's pointer */
            outfile.write(&data[0], nbytes);
            outfile.close();
            number_of_calls_++;
        } else {
            throw GadgetronClientException("Unable to write blob to output file\n");
        }
    }

protected:
    size_t number_of_calls_;
    std::string file_prefix;
    std::string file_suffix;

};

class GadgetronClientBlobAttribMessageReader 
    : public GadgetronClientMessageReader
{

public:
    GadgetronClientBlobAttribMessageReader(std::string fileprefix, std::string filesuffix)
        : number_of_calls_(0)
        , file_prefix(fileprefix)
        , file_suffix(filesuffix)

    {

    }

    virtual ~GadgetronClientBlobAttribMessageReader() {}

    virtual void read(tcp::socket* socket) 
    {

        // MUST READ 32-bits
        uint32_t nbytes;
        boost::asio::read(*socket, boost::asio::buffer(&nbytes,sizeof(uint32_t)));

        std::vector<char> data(nbytes,0);
        boost::asio::read(*socket, boost::asio::buffer(&data[0],nbytes));


        unsigned long long fileNameLen;
        boost::asio::read(*socket, boost::asio::buffer(&fileNameLen,sizeof(unsigned long long)));

        std::string filenameBuf(fileNameLen,0);
        boost::asio::read(*socket, boost::asio::buffer(const_cast<char*>(filenameBuf.c_str()),fileNameLen));

        typedef unsigned long long size_t_type;

        size_t_type meta_attrib_length;
        boost::asio::read(*socket, boost::asio::buffer(&meta_attrib_length, sizeof(size_t_type)));

        std::string meta_attrib(meta_attrib_length-sizeof(size_t_type),0);
        boost::asio::read(*socket, boost::asio::buffer(const_cast<char*>(meta_attrib.c_str()), meta_attrib_length-sizeof(size_t_type)));


        std::string filename_image, filename_attrib;

        // Create the filename: (prefix_%06.suffix)
        if ( file_prefix.empty() )
        {
            filename_image =  filenameBuf + "." + file_suffix;
            filename_attrib =  filenameBuf + "_attrib.xml";
        }
        else
        {
            filename_image = file_prefix + "_" + filenameBuf + "." + file_suffix;
            filename_attrib = file_prefix + "_" + filenameBuf + "_attrib.xml";
        }

        std::cout << "Writing image " << filename_image.c_str() << std::endl;

        std::ofstream outfile;
        outfile.open (filename_image.c_str(), std::ios::out|std::ios::binary);

        std::ofstream outfile_attrib;
        outfile_attrib.open (filename_attrib.c_str(), std::ios::out|std::ios::binary);

        if (outfile.good())
        {
            /* write 'size' bytes starting at 'data's pointer */
            outfile.write(&data[0], nbytes);
            outfile.close();

            outfile_attrib.write(meta_attrib.c_str(), meta_attrib.length());
            outfile_attrib.close();

            number_of_calls_++;
        }
        else
        {
            throw GadgetronClientException("Unable to write blob to output file\n");
        }
    }

protected:
    size_t number_of_calls_;
    std::string file_prefix;
    std::string file_suffix;

};

class GadgetronClientConnector
{

public:
    GadgetronClientConnector() 
        : socket_(0)
    {

    }

    virtual ~GadgetronClientConnector() 
    {
        if (socket) {
            socket_->close();
            delete socket_;
        }
    }

    void read_task()
    {
        if (!socket_) {
            throw GadgetronClientException("Unable to create socket.");
        }

        GadgetMessageIdentifier id;
        while (socket_->is_open()) {
            boost::asio::read(*socket_, boost::asio::buffer(&id,sizeof(GadgetMessageIdentifier)));

            if (id.id == GADGET_MESSAGE_CLOSE) {
                break;
            }

            GadgetronClientMessageReader* r = find_reader(id.id);

            if (!r) {
                std::cout << "Message received with ID: " << id.id << std::endl;
                throw GadgetronClientException("Unknown Message ID");
            } else {
                r->read(socket_);
            }
        }
    }

    void wait() {
        reader_thread_.join();
    }

    void connect(std::string hostname, std::string port)
    {


        tcp::resolver resolver(io_service);
        tcp::resolver::query query(tcp::v4(), hostname.c_str(), port.c_str());
        tcp::resolver::iterator endpoint_iterator = resolver.resolve(query);
        tcp::resolver::iterator end;

        socket_ = new tcp::socket(io_service);

        if (!socket_) {
            throw GadgetronClientException("Unable to create socket.");
        }

        //TODO:
        //For newer versions of Boost, we should use
        //   boost::asio::connect(*socket_, iterator);

        boost::system::error_code error = boost::asio::error::host_not_found;
        while (error && endpoint_iterator != end) {
            socket_->close();
            socket_->connect(*endpoint_iterator++, error);
        }
        if (error)
            throw GadgetronClientException("Error connecting using socket.");

        reader_thread_ = boost::thread(boost::bind(&GadgetronClientConnector::read_task, this));

    }

    void send_gadgetron_close() { 
        if (!socket_) {
            throw GadgetronClientException("Invalid socket.");
        }
        GadgetMessageIdentifier id;
        id.id = GADGET_MESSAGE_CLOSE;    
        boost::asio::write(*socket_, boost::asio::buffer(&id, sizeof(GadgetMessageIdentifier)));
    }

    void send_gadgetron_configuration_file(std::string config_xml_name) {

        if (!socket_) {
            throw GadgetronClientException("Invalid socket.");
        }

        GadgetMessageIdentifier id;
        id.id = GADGET_MESSAGE_CONFIG_FILE;

        GadgetMessageConfigurationFile ini;
        memset(&ini,0,sizeof(GadgetMessageConfigurationFile));
        strncpy(ini.configuration_file, config_xml_name.c_str(),config_xml_name.size());

        boost::asio::write(*socket_, boost::asio::buffer(&id, sizeof(GadgetMessageIdentifier)));
        boost::asio::write(*socket_, boost::asio::buffer(&ini, sizeof(GadgetMessageConfigurationFile)));

    }

    void send_gadgetron_configuration_script(std::string xml_string)
    {
        if (!socket_) {
            throw GadgetronClientException("Invalid socket.");
        }

        GadgetMessageIdentifier id;
        id.id = GADGET_MESSAGE_CONFIG_SCRIPT;

        GadgetMessageScript conf;
        conf.script_length = (uint32_t)xml_string.size()+1;

        boost::asio::write(*socket_, boost::asio::buffer(&id, sizeof(GadgetMessageIdentifier)));
        boost::asio::write(*socket_, boost::asio::buffer(&conf, sizeof(GadgetMessageScript)));
        boost::asio::write(*socket_, boost::asio::buffer(xml_string.c_str(), conf.script_length));    

    }


    void  send_gadgetron_parameters(std::string xml_string)
    {
        if (!socket_) {
            throw GadgetronClientException("Invalid socket.");
        }

        GadgetMessageIdentifier id;
        id.id = GADGET_MESSAGE_PARAMETER_SCRIPT;

        GadgetMessageScript conf;
        conf.script_length = (uint32_t)xml_string.size()+1;

        boost::asio::write(*socket_, boost::asio::buffer(&id, sizeof(GadgetMessageIdentifier)));
        boost::asio::write(*socket_, boost::asio::buffer(&conf, sizeof(GadgetMessageScript)));
        boost::asio::write(*socket_, boost::asio::buffer(xml_string.c_str(), conf.script_length));    
    }

    void send_ismrmrd_acquisition(ISMRMRD::Acquisition& acq) 
    {
        if (!socket_) {
            throw GadgetronClientException("Invalid socket.");
        }

        GadgetMessageIdentifier id;
        id.id = GADGET_MESSAGE_ISMRMRD_ACQUISITION;;

        boost::asio::write(*socket_, boost::asio::buffer(&id, sizeof(GadgetMessageIdentifier)));
        boost::asio::write(*socket_, boost::asio::buffer(&acq.getHead(), sizeof(ISMRMRD::AcquisitionHeader)));

        unsigned long trajectory_elements = acq.getHead().trajectory_dimensions*acq.getHead().number_of_samples;
        unsigned long data_elements = acq.getHead().active_channels*acq.getHead().number_of_samples;

        if (trajectory_elements) {
            boost::asio::write(*socket_, boost::asio::buffer(&acq.getTraj()[0], sizeof(float)*trajectory_elements));
        }


        if (data_elements) {
            boost::asio::write(*socket_, boost::asio::buffer(&acq.getData()[0], 2*sizeof(float)*data_elements));
        }
    }

    void register_reader(unsigned short slot, boost::shared_ptr<GadgetronClientMessageReader> r) {
        readers_[slot] = r;
    }

protected:
    typedef std::map<unsigned short, boost::shared_ptr<GadgetronClientMessageReader> > maptype;

    GadgetronClientMessageReader* find_reader(unsigned short r)
    {
        GadgetronClientMessageReader* ret = 0;

        maptype::iterator it = readers_.find(r);

        if (it != readers_.end()) {
            ret = it->second.get();
        }

        return ret;
    }

    boost::asio::io_service io_service;
    tcp::socket* socket_;
    boost::thread reader_thread_;
    maptype readers_;


};


int main(int argc, char **argv)
{

    std::string host_name;
    std::string port;
    std::string in_filename;
    std::string out_filename;
    std::string hdf5_in_group;
    std::string hdf5_out_group;
    std::string config_file;
    std::string config_file_local;
    std::string config_xml_local;
    unsigned int loops;

    po::options_description desc("Allowed options");

    desc.add_options()
        ("help,h", "produce help message")
        ("port,p", po::value<std::string>(&port)->default_value("9002"), "Port")
        ("address,a", po::value<std::string>(&host_name)->default_value("localhost"), "Address (hostname) of Gadgetron host")
        ("filename,f", po::value<std::string>(&in_filename), "Input file")
        ("outfile,o", po::value<std::string>(&out_filename)->default_value("out.h5"), "Output file")
        ("in-group,g", po::value<std::string>(&hdf5_in_group)->default_value("/dataset"), "Input data group")
        ("out-group,G", po::value<std::string>(&hdf5_out_group)->default_value(get_date_time_string()), "Output group name")  
        ("config,c", po::value<std::string>(&config_file)->default_value("default.xml"), "Configuration file (remote)")
        ("config-local,C", po::value<std::string>(&config_file_local), "Configuration file (local)")
        ("loops,l", po::value<unsigned int>(&loops)->default_value(1), "Loops")
        ;

    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);

    if (vm.count("help")) {
        std::cout << desc << std::endl;
        return 0;
    }

    if (!vm.count("filename")) {
        std::cout << std::endl << std::endl << "\tYou must supply a filename" << std::endl << std::endl;
        std::cout << desc << std::endl;
        return -1;
    }

    if (vm.count("config-local")) {
        std::ifstream t(config_file_local.c_str());
        if (t) {
            //Read in the file.
            config_xml_local = std::string((std::istreambuf_iterator<char>(t)),
                std::istreambuf_iterator<char>());
        } else {
            std::cout << "Unable to read local xml configuration: " << config_file_local  << std::endl;
            return -1;
        }
    }

    std::cout << "Gadgetron ISMRMRD client" << std::endl;

    //Let's check if the files exist:
    std::string hdf5_xml_varname = std::string(hdf5_in_group) + std::string("/xml");
    std::string hdf5_data_varname = std::string(hdf5_in_group) + std::string("/data");


    //TODO:
    // Add check to see if input file exists

    //Let's open the input file
    ISMRMRD::Dataset ismrmrd_dataset(in_filename.c_str(), hdf5_in_group.c_str(), false);
    // Read the header
    std::string xml_config;
    ismrmrd_dataset.readHeader(xml_config);


    std::cout << "  -- host            :      " << host_name << std::endl;
    std::cout << "  -- port            :      " << port << std::endl;
    std::cout << "  -- hdf5 file  in   :      " << in_filename << std::endl;
    std::cout << "  -- hdf5 group in   :      " << hdf5_in_group << std::endl;
    std::cout << "  -- conf            :      " << config_file << std::endl;
    std::cout << "  -- loop            :      " << loops << std::endl;
    std::cout << "  -- hdf5 file out   :      " << out_filename << std::endl;
    std::cout << "  -- hdf5 group out  :      " << hdf5_out_group << std::endl;


    GadgetronClientConnector con;

    con.register_reader(GADGET_MESSAGE_ISMRMRD_IMAGE_REAL_USHORT, boost::shared_ptr<GadgetronClientMessageReader>(new GadgetronClientImageMessageReader<uint16_t>(out_filename, hdf5_out_group)));
    con.register_reader(GADGET_MESSAGE_ISMRMRD_IMAGE_REAL_FLOAT, boost::shared_ptr<GadgetronClientMessageReader>(new GadgetronClientImageMessageReader<float>(out_filename, hdf5_out_group)));
    con.register_reader(GADGET_MESSAGE_ISMRMRD_IMAGE_CPLX_FLOAT, boost::shared_ptr<GadgetronClientMessageReader>(new GadgetronClientImageMessageReader< std::complex<float> >(out_filename, hdf5_out_group)));

    //Image with attributes 
    con.register_reader(GADGET_MESSAGE_ISMRMRD_IMAGEWITHATTRIB_REAL_USHORT, boost::shared_ptr<GadgetronClientMessageReader>(new GadgetronClientAttribImageMessageReader<uint16_t>(out_filename, hdf5_out_group)));
    con.register_reader(GADGET_MESSAGE_ISMRMRD_IMAGEWITHATTRIB_REAL_FLOAT, boost::shared_ptr<GadgetronClientMessageReader>(new GadgetronClientAttribImageMessageReader<float>(out_filename, hdf5_out_group)));
    con.register_reader(GADGET_MESSAGE_ISMRMRD_IMAGEWITHATTRIB_CPLX_FLOAT, boost::shared_ptr<GadgetronClientMessageReader>(new GadgetronClientAttribImageMessageReader< std::complex<float> >(out_filename, hdf5_out_group)));

    con.register_reader(GADGET_MESSAGE_DICOM, boost::shared_ptr<GadgetronClientMessageReader>(new GadgetronClientBlobMessageReader(std::string(hdf5_out_group), std::string("dcm"))));
    con.register_reader(GADGET_MESSAGE_DICOM_WITHNAME, boost::shared_ptr<GadgetronClientMessageReader>(new GadgetronClientBlobAttribMessageReader(std::string(), std::string("dcm"))));

    try {
        con.connect(host_name,port);
        if (vm.count("config-local")) {
            con.send_gadgetron_configuration_script(config_xml_local);
        } else {
            con.send_gadgetron_configuration_file(config_file);
        }
        con.send_gadgetron_parameters(xml_config);

        uint32_t acquisitions = 0;
        {
            mtx.lock();
            acquisitions = ismrmrd_dataset.getNumberOfAcquisitions();
            mtx.unlock();
        }

        ISMRMRD::Acquisition acq_tmp;
        for (uint32_t i = 0; i < acquisitions; i++) {
            {
                {
                    boost::mutex::scoped_lock scoped_lock(mtx);
                    ismrmrd_dataset.readAcquisition(i, acq_tmp);
                }
                con.send_ismrmrd_acquisition(acq_tmp);
            }
        }

        con.send_gadgetron_close();
        con.wait();

    } catch (std::exception& ex) {
        std::cout << "Error caught: " << ex.what() << std::endl;
    }

    return 0;
}
