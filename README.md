CardViewLibrary
===============

CardView Library for Android L

This project can be downloaded and Added into your eclipse workspace as a dependency for
a project targeted at Android L developer Preview.

In order to use this project you must download the Android L Developer Preview SDK from the
SDK Manager within Eclipse, Android Studio or IntelliJ.

After downloading the SDK, navigate to your android-sdk-macosx/extras/android/m2repository/android/support folder. 
Inside this folder you will see various folders, for the new SDK.

Next Steps:

Navigate to cardview-v7/21.0.0-rc01 folder, and find the cardview-v7-21.0.0-rc1.aar file.

cp this file as cardview-v7-21.0.0-rc1.zip and extract the zip file.

It is easiest to do this from the command line:

cp cardview-v7-21.0.0-rc1.aar cardview-v7-21.0.0-rc1.zip


Double click the new file once its created and it should unzip.

You are now ready to download this CardViewLibrary and add it into eclipse as Existing Project.

Be sure to mark the downloaded CardViewLibrary as a library project in eclipse, And make sure your
targeting Android L.


Find the classes.jar file located in the new cardview-v7-21.0.0-rc1 folder, copy this folder
and add it as a dependency to the Project.

You are now ready to use the CardView introduced in Android L.


Some Additional requirments:

As of now its best to use the CardView with either a GridLayout or Create an Adapter and Add multiple card
views to this adapter. 

See the AndroidLBeta Project in my repo list for examples.

GoodLuck