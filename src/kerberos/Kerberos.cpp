#include "Kerberos.h"

namespace kerberos
{
    void Kerberos::bootstrap(StringMap & parameters)
    {
        // --------------------------------
        // Set parameters from command-line

        setParameters(parameters);

        // ---------------------
        // initialize RestClient

        RestClient::init();

        // ---------------------
        // Initialize kerberos

        std::string configuration = (helper::getValueByKey(parameters, "config")) ?: CONFIGURATION_PATH;
        configure(configuration);

        // ------------------------------------------
        // Guard is a filewatcher, that looks if the
        // configuration has been changed. On change
        // guard will re-configure all instances.

        std::string directory = configuration.substr(0, configuration.rfind('/'));
        std::string file = configuration.substr(configuration.rfind('/')+1);
        guard = new FW::Guard();
        guard->listenTo(directory, file);
        guard->onChange(&Kerberos::reconfigure);
        guard->start();

        // --------------------------
        // This should be forever...

        while(true)
        {
            // -------------------
            // Initialize data

            JSON data;
            data.SetObject();

            // ------------------------------------
            // Guard look if the configuration has
            // been changed...

            guard->look();

            // --------------------------------------------
            // If machinery is NOT allowed to do detection
            // continue iteration

            if(!machinery->allowed(m_images))
            {
                VLOG(1) << "Machinery on hold, conditions failed.";
                continue;
            }

            // --------------------
            // Clean image to save

            Image cleanImage = *m_images[m_images.size()-1];

            // --------------
            // Processing..

            if(machinery->detect(m_images, data))
            {
                // ---------------------------
                // If something is detected...

                pthread_mutex_lock(&m_ioLock);

                Detection detection(toJSON(data), cleanImage);
                m_detections.push_back(detection);

                // -----------------------------------------------
                // If we have a cloud account, send a notification
                // to the cloud app.

                if(cloud && cloud->m_publicKey != "")
                {
                    cloud->fstream.triggerMotion();
                }

                pthread_mutex_unlock(&m_ioLock);
            }

            // -------------
            // Shift images

            m_images = capture->shiftImage();
        }
    }

    std::string Kerberos::toJSON(JSON & data)
    {
        JSON::AllocatorType& allocator = data.GetAllocator();

        JSONValue name;
        name.SetString(m_name.c_str(), allocator);
        data.AddMember("name", name, allocator);

        JSONValue timestamp;
        timestamp.SetString(kerberos::helper::getTimestamp().c_str(), allocator);
        data.AddMember("timestamp", timestamp, allocator);

        std::string micro = kerberos::helper::getMicroseconds();
        micro = kerberos::helper::to_string((int)micro.length()) + "-" + micro;

        JSONValue microseconds;
        microseconds.SetString(micro.c_str(), allocator);
        data.AddMember("microseconds", microseconds, allocator);

        data.AddMember("token", rand()%1000, allocator);

        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
        data.Accept(writer);
        return buffer.GetString();
    }

    void Kerberos::configure(const std::string & configuration)
    {
        // ---------------------------
    	// Get settings from XML file

        VLOG(1) << "Reading configuration file: " << configuration;
        StringMap settings = kerberos::helper::getSettingsFromXML(configuration);
        settings["configuration"] = configuration;

        // -------------------------------
        // Override config with parameters

        StringMap parameters = getParameters();
        StringMap::iterator begin = parameters.begin();
        StringMap::iterator end = parameters.end();

        for(begin; begin != end; begin++)
        {
            settings[begin->first] = begin->second;
        }

        VLOG(1) << helper::printStringMap("Final configuration:", settings);

        // -----------------
        // Get instance name

        m_name = settings.at("name");

        // -------------------------------------------
        // Check if we need to disable verbose logging

        if(settings.at("logging") == "false")
        {
            VLOG(1) << "Logging is set to info";
            el::Loggers::setVerboseLevel(1);
        }
        else
        {
            VLOG(1) << "Logging is set to verbose";
            el::Loggers::setVerboseLevel(2);
        }

        // ------------------
        // Configure capture

        configureCapture(settings);

        // -----------------
        // Configure cloud

        configureCloud(settings);

        // ------------------
        // Stop the io thread

        if(m_ioThread_running)
        {
            stopIOThread();
        }

        // --------------------
        // Initialize machinery

        if(machinery != 0) delete machinery;
        machinery = new Machinery();
        machinery->setCapture(capture);
        machinery->setup(settings);

        // ------------------
        // Open the io thread

        startIOThread();

        // -------------------
        // Take first images

        for(ImageVector::iterator it = m_images.begin(); it != m_images.end(); it++)
        {
            delete *it;
        }

        m_images.clear();
        m_images = capture->takeImages(3);

        machinery->initialize(m_images);
    }

    // ----------------------------------
    // Configure capture device + stream

    void Kerberos::configureCapture(StringMap & settings)
    {
        // -----------------------
        // Stop stream and capture

        if(stream != 0)
        {
            VLOG(1) << "Steam: Stopping streaming thread";
            stopStreamThread();
            delete stream;
            stream = 0;
        }

        if(capture != 0)
        {
            VLOG(1) << "Capture: Stop capture device";
            if(capture->isOpened())
            {
                VLOG(2) << "Capture: Disable capture device in machinery";
                machinery->disableCapture();
                VLOG(2) << "Capture: Stop cloud live streaming";
                cloud->stopLivestreamThread();
                VLOG(2) << "Capture: Disable capture device in cloud";
                cloud->disableCapture();
                VLOG(2) << "Capture: Stop capture grab thread";
                capture->stopGrabThread();
                VLOG(2) << "Capture: Stop capture health thread";
                capture->stopHealthThread();
                VLOG(2) << "Capture: Close capture device";
                capture->close();
            }
            delete capture;
            capture = 0;
        }

        // ---------------------------
        // Initialize capture device

        VLOG(1) << "Capture: Start capture device: " + settings.at("capture");
        capture = Factory<Capture>::getInstance()->create(settings.at("capture"));
        capture->setup(settings);
        VLOG(2) << "Capture: Start capture grab thread";
        capture->startGrabThread();
        VLOG(2) << "Capture: Start capture health thread";
        capture->startHealthThread();

        // ------------------
        // Initialize stream

        usleep(1000*5000);
        stream = new Stream();
        stream->configureStream(settings);
        VLOG(1) << "Capture: Start streaming thread";
        startStreamThread();
    }

    // ----------------------------------
    // Configure cloud device + thread

    void Kerberos::configureCloud(StringMap & settings)
    {
        // ---------------------------
        // Initialize cloud service

        if(cloud != 0)
        {
            VLOG(1) << "Cloud: Stop cloud service";
            VLOG(2) << "Cloud: Stop upload thread";
            cloud->stopUploadThread();
            VLOG(2) << "Cloud: Stop polling thread";
            cloud->stopPollThread();
            VLOG(2) << "Cloud: Stop health thread";
            cloud->stopHealthThread();
            delete cloud;
        }

        VLOG(1) << "Starting cloud service: " + settings.at("cloud");
        cloud = Factory<Cloud>::getInstance()->create(settings.at("cloud"));
        cloud->setCapture(capture);
        cloud->setup(settings);
    }

    // --------------------------------------------
    // Function ran in a thread, which continuously
    // stream MJPEG's.

    void * streamContinuously(void * self)
    {
        Kerberos * kerberos = (Kerberos *) self;

        uint8_t * data = new uint8_t[(int)(1280*960*1.5)];
        int32_t length = kerberos->capture->retrieveRAW(data);

        while(kerberos->m_streamThread_running &&
              kerberos->stream->isOpened())
        {
            try
            {
                kerberos->stream->connect();

                if(kerberos->stream->hasClients())
                {
                    if(kerberos->capture->m_hardwareMJPEGEncoding)
                    {
                        length = kerberos->capture->retrieveRAW(data);
                        kerberos->stream->writeRAW(data, length);
                    }
                    else
                    {
                        Image image = kerberos->capture->retrieve();
                        if(kerberos->capture->m_angle != 0)
                        {
                            image.rotate(kerberos->capture->m_angle);
                        }
                        kerberos->stream->write(image);
                    }
                }

                usleep(kerberos->stream->wait * 1000 * 1000); // sleep x microsec.
            }
            catch(cv::Exception & ex){}
        }

        delete data;
    }

    void Kerberos::startStreamThread()
    {
        // ------------------------------------------------
        // Start a new thread that streams MJPEG's continuously.

        if(stream != 0)
        {
            //if stream object just exists try to open configured stream port
            stream->open();
        }

        m_streamThread_running = true;
        pthread_create(&m_streamThread, NULL, streamContinuously, this);
    }

    void Kerberos::stopStreamThread()
    {
        // ----------------------------------
        // Cancel the existing stream thread,

        m_streamThread_running = false;
        pthread_cancel(m_streamThread);
        pthread_join(m_streamThread, NULL);
    }

    // -------------------------------------------
    // Function ran in a thread, which continuously
    // checks if some detections occurred and
    // execute the IO devices if so.

    void * checkDetectionsContinuously(void * self)
    {
        Kerberos * kerberos = (Kerberos *) self;

        int previousCount = 0;
        int currentCount = 0;
        int timesEqual = 0;

        while(kerberos->m_ioThread_running)
        {
            try
            {
                previousCount = currentCount;
                currentCount = kerberos->m_detections.size();

                if(previousCount == currentCount)
                {
                    timesEqual++;
                }
                else if(timesEqual > 0)
                {
                    timesEqual--;
                }

                // If no new detections are found, we will run the IO devices (or max 30 images in memory)
                if((currentCount > 0 && timesEqual > 4) || currentCount >= 30)
                {
                    VLOG(1) << "Executing IO devices for " + helper::to_string(currentCount)  + " detection(s)";

                    for (int i = 0; i < currentCount; i++)
                    {
                        Detection detection = kerberos->m_detections[0];
                        JSON data;
                        data.Parse(detection.t.c_str());

                        pthread_mutex_lock(&kerberos->m_ioLock);
                        if(kerberos->machinery->save(detection.k, data))
                        {
                            kerberos->m_detections.erase(kerberos->m_detections.begin());
                        }
                        else
                        {
                            LOG(ERROR) << "IO: can't execute";
                        }
                        pthread_mutex_unlock(&kerberos->m_ioLock);
                        usleep(500*1000);
                    }

                    timesEqual = 0;
                }

                usleep(500*1000);
            }
            catch(cv::Exception & ex)
            {
                pthread_mutex_unlock(&kerberos->m_ioLock);
            }
        }
    }

    void Kerberos::startIOThread()
    {
        // ------------------------------------------------
        // Start a new thread that cheks for detections

        m_ioThread_running = true;
        pthread_create(&m_ioThread, NULL, checkDetectionsContinuously, this);
    }

    void Kerberos::stopIOThread()
    {
        // ----------------------------------
        // Cancel the existing io thread,

        m_ioThread_running = false;
        pthread_cancel(m_ioThread);
        pthread_join(m_ioThread, NULL);
    }
}
