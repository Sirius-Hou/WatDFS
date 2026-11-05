#include "rw_lock.h"

#include <cerrno>

int rw_lock_init(rw_lock_t *lock) {
    int rc = pthread_mutex_init(&lock->mutex_, nullptr);
    if (rc != 0) {
        return -rc;
    }

    rc = pthread_cond_init(&lock->cv_, nullptr);
    if (rc != 0) {
        pthread_mutex_destroy(&lock->mutex_);
        return -rc;
    }

    lock->num_readers_ = 0;
    lock->num_writers_ = 0;
    lock->num_waiting_writers_ = 0;
    return 0;
}

int rw_lock_destroy(rw_lock_t *lock) {
    int rc = pthread_mutex_destroy(&lock->mutex_);
    if (rc != 0) {
        return -rc;
    }

    rc = pthread_cond_destroy(&lock->cv_);
    if (rc != 0) {
        return -rc;
    }
    return 0;
}

int rw_lock_lock(rw_lock_t *lock, rw_lock_mode_t mode) {
    int rc = pthread_mutex_lock(&lock->mutex_);
    if (rc != 0) {
        return -rc;
    }

    if (mode == RW_READ_LOCK) {
        while (lock->num_writers_ > 0 || lock->num_waiting_writers_ > 0) {
            rc = pthread_cond_wait(&lock->cv_, &lock->mutex_);
            if (rc != 0) {
                pthread_mutex_unlock(&lock->mutex_);
                return -rc;
            }
        }
        ++lock->num_readers_;
    } else if (mode == RW_WRITE_LOCK) {
        ++lock->num_waiting_writers_;
        while (lock->num_readers_ > 0 || lock->num_writers_ > 0) {
            rc = pthread_cond_wait(&lock->cv_, &lock->mutex_);
            if (rc != 0) {
                --lock->num_waiting_writers_;
                pthread_mutex_unlock(&lock->mutex_);
                return -rc;
            }
        }
        --lock->num_waiting_writers_;
        lock->num_writers_ = 1;
    } else {
        pthread_mutex_unlock(&lock->mutex_);
        return -EINVAL;
    }

    rc = pthread_mutex_unlock(&lock->mutex_);
    if (rc != 0) {
        return -rc;
    }
    return 0;
}

int rw_lock_unlock(rw_lock_t *lock, rw_lock_mode_t mode) {
    int rc = pthread_mutex_lock(&lock->mutex_);
    if (rc != 0) {
        return -rc;
    }

    if (mode == RW_READ_LOCK) {
        if (lock->num_readers_ <= 0) {
            pthread_mutex_unlock(&lock->mutex_);
            return -EPERM;
        }
        --lock->num_readers_;
    } else if (mode == RW_WRITE_LOCK) {
        if (lock->num_writers_ != 1) {
            pthread_mutex_unlock(&lock->mutex_);
            return -EPERM;
        }
        lock->num_writers_ = 0;
    } else {
        pthread_mutex_unlock(&lock->mutex_);
        return -EINVAL;
    }

    rc = pthread_cond_broadcast(&lock->cv_);
    if (rc != 0) {
        pthread_mutex_unlock(&lock->mutex_);
        return -rc;
    }

    rc = pthread_mutex_unlock(&lock->mutex_);
    if (rc != 0) {
        return -rc;
    }
    return 0;
}
