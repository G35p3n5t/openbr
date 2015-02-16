#include "openbr_internal.h"
#include "openbr/core/qtutils.h"
#include "openbr/core/eigenutils.h"

#include <dlib/image_processing/frontal_face_detector.h>
#include <dlib/image_processing/render_face_detections.h>
#include <dlib/svm_threaded.h>
#include <dlib/image_processing.h>
#include <dlib/gui_widgets.h>
#include <dlib/image_io.h>
#include <dlib/opencv.h>

#include <QTemporaryFile>

using namespace std;
using namespace dlib;

namespace br
{

class DLibShapeResourceMaker : public ResourceMaker<shape_predictor>
{

private:
    shape_predictor *make() const
    {
        shape_predictor *sp = new shape_predictor();
        dlib::deserialize(qPrintable(Globals->sdkPath + "/share/openbr/models/dlib/shape_predictor_68_face_landmarks.dat")) >> *sp;
        return sp;
    }
};

class DLandmarkerTransform : public UntrainableTransform
{
    Q_OBJECT

private:

    Resource<shape_predictor> shapeResource;

    void init()
    {
        shapeResource.setResourceMaker(new DLibShapeResourceMaker());
    }

    void project(const Template &src, Template &dst) const
    {
        dst = src;

        if (!src.file.rects().isEmpty()) {
            shape_predictor *sp = shapeResource.acquire();

            cv_image<bgr_pixel> cimg(src.m().clone());

            for (unsigned long j = 0; j < src.file.rects().size(); ++j)
            {
                QRectF rect = src.file.rects()[j];
                rectangle r(rect.left(),rect.top(),rect.right(),rect.bottom());
                full_object_detection shape = (*sp)(cimg, r);
                for (int i=0; i<shape.num_parts(); i++)
                    dst.file.appendPoint(QPointF(shape.part(i)(0),shape.part(i)(1)));
            }

            shapeResource.release(sp);
        }
    }
};

BR_REGISTER(Transform, DLandmarkerTransform)

class DObjectDetectTransform : public Transform
{
    Q_OBJECT

    Q_PROPERTY(int winSize READ get_winSize WRITE set_winSize RESET reset_winSize STORED true)
    Q_PROPERTY(float C READ get_C WRITE set_C RESET reset_C STORED true)
    Q_PROPERTY(float epsilon READ get_epsilon WRITE set_epsilon RESET reset_epsilon STORED true)
    BR_PROPERTY(int, winSize, 80)
    BR_PROPERTY(float, C, 1)
    BR_PROPERTY(float, epsilon, .01)

private:
    typedef scan_fhog_pyramid<pyramid_down<6> > image_scanner_type;
    mutable object_detector<image_scanner_type> detector;

    void train(const TemplateList &data)
    {
        dlib::array<array2d<unsigned char> > samples;
        std::vector<std::vector<rectangle> > boxes;

        foreach (const Template &t, data) {
            if (!t.file.rects().isEmpty()) {
                cv_image<bgr_pixel> cimg(t.m().clone());

                array2d<unsigned char> image;
                assign_image(image,cimg);

                samples.push_back(image);

                std::vector<rectangle> b;
                foreach (const QRectF &r, t.file.rects())
                    b.push_back(rectangle(r.left(),r.top(),r.right(),r.bottom()));

                boxes.push_back(b);
            }
        }

        add_image_left_right_flips(samples, boxes);

        image_scanner_type scanner;

        scanner.set_detection_window_size(winSize, winSize);
        structural_object_detection_trainer<image_scanner_type> trainer(scanner);
        trainer.set_num_threads(max(1,QThread::idealThreadCount()));
        trainer.set_c(C);
        trainer.set_epsilon(epsilon);

        detector = trainer.train(samples, boxes);
    }

    void project(const Template &src, Template &dst) const
    {
        dst = src;
        cv_image<bgr_pixel> cimg(src.m().clone());
        array2d<unsigned char> image;
        assign_image(image,cimg);

        std::vector<rectangle> dets = detector(image);

        for (int i=0; i<dets.size(); i++)
            dst.file.appendRect(QRectF(QPointF(dets[i].left(),dets[i].top()),QPointF(dets[i].right(),dets[i].bottom())));
    }

    void store(QDataStream &stream) const
    {
        // Create local file
        QTemporaryFile tempFile;
        tempFile.open();
        tempFile.close();

        dlib::serialize(qPrintable(tempFile.fileName())) << detector;

        // Copy local file contents to stream
        tempFile.open();
        QByteArray data = tempFile.readAll();
        tempFile.close();
        stream << data;
    }

    void load(QDataStream &stream)
    {
        // Copy local file contents from stream
        QByteArray data;
        stream >> data;

        // Create local file
        QTemporaryFile tempFile(QDir::tempPath()+"/model");
        tempFile.open();
        tempFile.write(data);
        tempFile.close();

        // Load MLP from local file
        dlib::deserialize(qPrintable(tempFile.fileName())) >> detector;
    }
};

BR_REGISTER(Transform, DObjectDetectTransform)

} // namespace br

#include "dlib.moc"