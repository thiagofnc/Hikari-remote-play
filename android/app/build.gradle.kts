plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("org.jetbrains.kotlin.plugin.compose")
}

// Release builds pass -PhikariVersion=1.2.3 (from the tag); versionCode follows it.
val hikariVersion = (findProperty("hikariVersion") as String?) ?: "0.1.10"
val hikariVersionCode = hikariVersion.substringBefore('-').split('.').map { it.toIntOrNull() ?: 0 }
    .let { (it.getOrElse(0) { 0 } * 10000) + (it.getOrElse(1) { 0 } * 100) + it.getOrElse(2) { 0 } }

// A stable release key keeps updates installable over older versions.
// CI provides it through HIKARI_KEYSTORE*; local builds fall back to the debug key.
val releaseKeystore = System.getenv("HIKARI_KEYSTORE")?.let { file(it) }?.takeIf { it.exists() }

android {
    namespace = "com.hikari.remoteplay"
    compileSdk = 35

    defaultConfig {
        applicationId = "com.hikari.remoteplay"
        minSdk = 26
        targetSdk = 35
        versionCode = hikariVersionCode
        versionName = hikariVersion
    }

    signingConfigs {
        if (releaseKeystore != null) {
            create("release") {
                storeFile = releaseKeystore
                storePassword = System.getenv("HIKARI_KEYSTORE_PASSWORD")
                keyAlias = System.getenv("HIKARI_KEY_ALIAS") ?: "hikari"
                keyPassword = System.getenv("HIKARI_KEY_PASSWORD") ?: System.getenv("HIKARI_KEYSTORE_PASSWORD")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
            signingConfig = signingConfigs.findByName("release") ?: signingConfigs.getByName("debug")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }
    buildFeatures {
        compose = true
    }
    // Lets JVM tests run the network code (android.util.Log etc. become no-ops).
    testOptions {
        unitTests.isReturnDefaultValues = true
    }
}

dependencies {
    val composeBom = platform("androidx.compose:compose-bom:2024.12.01")
    implementation(composeBom)
    implementation("androidx.core:core-ktx:1.15.0")
    implementation("androidx.activity:activity-compose:1.9.3")
    implementation("androidx.lifecycle:lifecycle-runtime-compose:2.8.7")
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.foundation:foundation")
    implementation("androidx.compose.material3:material3")
    testImplementation("junit:junit:4.13.2")
}
