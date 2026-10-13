#import <AVFoundation/AVFoundation.h>
#include <QCoreApplication>
#include <QMetaObject>
#include <functional>

void requestMicrophonePermission(std::function<void(bool)> completion) {
    [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio completionHandler:^(BOOL granted) {
        QMetaObject::invokeMethod(QCoreApplication::instance(),[completion,granted] {completion(granted);},Qt::QueuedConnection);
    }];
}
